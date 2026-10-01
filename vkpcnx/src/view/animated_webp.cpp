#include "view/animated_webp.hpp"

#include "core/http.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>

namespace vkpcnx {

namespace {
// Frames with a zero/tiny delay would spin the timeline; browsers show them
// for a sane minimum too.
constexpr int kMinFrameDurationMs = 10;

std::string readBinaryFile(const std::string &path) {
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in.is_open()) {
    brls::Logger::error("AnimatedWebp: cannot open file: {}", path);
    return "";
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
} // namespace

AnimatedWebp::AnimatedWebp() {
  this->registerFilePathXMLAttribute("webp", [this](const std::string &value) {
    this->setWebpFromFile(value);
  });
  this->registerBoolXMLAttribute("loop", [this](bool value) { this->setLooping(value); });
  this->registerBoolXMLAttribute("autoplay", [this](bool value) {
    this->setAutoplay(value);
    if (value && this->decoder)
      this->play();
    else if (!value)
      this->pause();
  });
}

AnimatedWebp::~AnimatedWebp() { this->destroyDecoder(); }

void AnimatedWebp::destroyDecoder() {
  if (this->decoder) {
    WebPAnimDecoderDelete(this->decoder);
    this->decoder = nullptr;
  }
  this->info = {};
  this->playing = false;
}

void AnimatedWebp::clear() {
  this->destroyDecoder();
  this->data.clear();
  brls::Image::clear();
}

void AnimatedWebp::setWebpFromRes(const std::string &name) {
#ifdef USE_LIBROMFS
  auto file = romfs::get(name);
  this->setWebpFromMem(std::string((const char *)file.data(), file.size()));
#else
  this->setWebpFromFile(std::string(BRLS_RESOURCES) + name);
#endif
}

void AnimatedWebp::setWebpFromFile(const std::string &path) {
#ifdef USE_LIBROMFS
  if (path.rfind("@res/", 0) == 0)
    return this->setWebpFromRes(path.substr(5));
#endif
  std::string bytes = readBinaryFile(path);
  if (bytes.empty())
    return;
  this->setWebpFromMem(std::move(bytes));
}

void AnimatedWebp::setWebpFromMem(const unsigned char *data, size_t size) {
  this->setWebpFromMem(std::string((const char *)data, size));
}

void AnimatedWebp::setWebpFromMem(std::string bytes) {
  this->destroyDecoder();
  this->data = std::move(bytes);

  WebPData webpData { (const uint8_t *)this->data.data(), this->data.size() };
  WebPAnimDecoderOptions options;
  WebPAnimDecoderOptionsInit(&options);
  options.color_mode = MODE_RGBA; // straight alpha, same as nanovg's stb_image path
  options.use_threads = 0;

  this->decoder = WebPAnimDecoderNew(&webpData, &options);
  if (!this->decoder || !WebPAnimDecoderGetInfo(this->decoder, &this->info)) {
    brls::Logger::error("AnimatedWebp: not a valid WebP ({} bytes)", this->data.size());
    this->destroyDecoder();
    this->data.clear();
    return;
  }

  NVGcontext *vg = brls::Application::getNVGContext();

  // A texture the size of the canvas that every frame gets uploaded into
  // (the decoder always hands back the fully composited canvas).
  this->setFreeTexture(true);
  int tex = nvgCreateImageRGBA(
    vg, (int)this->info.canvas_width, (int)this->info.canvas_height, this->getImageFlags(), nullptr
  );
  if (tex == 0) {
    brls::Logger::error("AnimatedWebp: cannot create {}x{} texture",
      this->info.canvas_width, this->info.canvas_height);
    this->destroyDecoder();
    this->data.clear();
    return;
  }
  this->innerSetImage(tex);

  this->prevTimestampMs = 0;
  this->loopsDone = 0;
  this->advanceFrame(vg);
  this->frameShownAt = brls::getCPUTimeUsec();
  this->playing = this->autoplay && this->info.frame_count > 1;
}

void AnimatedWebp::setWebpAsync(std::function<void(std::function<void(const std::string &)>)> cb) {
  ASYNC_RETAIN
  cb([ASYNC_TOKEN](const std::string &bytes) {
    brls::sync([ASYNC_TOKEN, bytes]() {
      ASYNC_RELEASE
      if (bytes.empty())
        return;
      this->setWebpFromMem(bytes);
    });
  });
}

void AnimatedWebp::setWebpFromUrl(const std::string &url) {
  this->setWebpAsync([url](std::function<void(const std::string &)> setter) {
    Http::getAsync(url, [setter](Http::Response res) {
      if (!res.ok())
        return;
      setter(res.body);
    });
  });
}

void AnimatedWebp::play() {
  if (!this->decoder || this->info.frame_count <= 1)
    return;
  if (this->playing)
    return;
  // A finished non-looping animation starts over
  if (!WebPAnimDecoderHasMoreFrames(this->decoder))
    this->restart();
  this->frameShownAt = brls::getCPUTimeUsec();
  this->playing = true;
}

void AnimatedWebp::pause() { this->playing = false; }

void AnimatedWebp::restart() {
  if (!this->decoder)
    return;
  WebPAnimDecoderReset(this->decoder);
  this->prevTimestampMs = 0;
  this->loopsDone = 0;
  this->advanceFrame(brls::Application::getNVGContext());
  this->frameShownAt = brls::getCPUTimeUsec();
}

bool AnimatedWebp::advanceFrame(NVGcontext *vg) {
  if (!this->decoder || this->texture == 0)
    return false;

  if (!WebPAnimDecoderHasMoreFrames(this->decoder)) {
    // loop_count == 0 means "forever" in the container
    bool loopAgain = this->looping
      && (this->info.loop_count == 0 || ++this->loopsDone < (int)this->info.loop_count);
    if (!loopAgain)
      return false;
    WebPAnimDecoderReset(this->decoder);
    this->prevTimestampMs = 0;
  }

  uint8_t *rgba = nullptr;
  int timestampMs = 0;
  if (!WebPAnimDecoderGetNext(this->decoder, &rgba, &timestampMs)) {
    brls::Logger::error("AnimatedWebp: frame decode failed");
    return false;
  }

  nvgUpdateImage(vg, this->texture, rgba);

  int durationMs = std::max(timestampMs - this->prevTimestampMs, kMinFrameDurationMs);
  this->currentFrameDurationUs = (brls::Time)durationMs * 1000;
  this->prevTimestampMs = timestampMs;
  return true;
}

void AnimatedWebp::draw(
  NVGcontext *vg,
  float x,
  float y,
  float width,
  float height,
  brls::Style style,
  brls::FrameContext *ctx
) {
  if (this->playing) {
    brls::Time now = brls::getCPUTimeUsec();
    if (now - this->frameShownAt >= this->currentFrameDurationUs) {
      // Advance one frame per draw and resync the clock rather than trying
      // to catch up: frames can only be decoded sequentially anyway, and
      // after a stall a slowed-down animation beats a burst of decodes.
      if (this->advanceFrame(vg))
        this->frameShownAt = now;
      else
        this->playing = false;
    }
  }

  brls::Image::draw(vg, x, y, width, height, style, ctx);
}

brls::View *AnimatedWebp::create() { return new AnimatedWebp(); }

} // namespace vkpcnx
