#include "core/media/video_decoder.hpp"
#include "core/diag/log.hpp"

#include <borealis/core/logger.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

namespace vkpcnx::media {

#ifdef __SWITCH__
// nvtegra only copies frames out with the hardware engine when every
// destination plane address and pitch is 256-byte aligned. FFmpeg's default
// frame buffers aren't, so it fell back to a CPU copy from uncached memory on
// every frame (and logged a warning each time).
static constexpr size_t kTransferAlign = 256;

static AVBufferRef *allocTransferBuffer(void *, size_t size) {
  void *data = std::aligned_alloc(kTransferAlign, FFALIGN(size, kTransferAlign));
  if (!data)
    return nullptr;
  AVBufferRef *buf = av_buffer_create(
    static_cast<uint8_t *>(data), size, [](void *, uint8_t *p) { std::free(p); }, nullptr, 0
  );
  if (!buf)
    std::free(data);
  return buf;
}

bool VideoDecoder::allocTransferFrame(AVFrame *out, const AVFrame *hw) {
  auto *framesCtx = reinterpret_cast<AVHWFramesContext *>(hw->hw_frames_ctx->data);
  AVPixelFormat fmt = framesCtx->sw_format;

  int linesizes[4];
  if (av_image_fill_linesizes(linesizes, fmt, hw->width) < 0)
    return false;
  ptrdiff_t pitches[4];
  for (int i = 0; i < 4; i++) {
    linesizes[i] = FFALIGN(linesizes[i], static_cast<int>(kTransferAlign));
    pitches[i] = linesizes[i];
  }
  size_t sizes[4];
  if (av_image_fill_plane_sizes(sizes, fmt, hw->height, pitches) < 0)
    return false;
  size_t offsets[4], total = 0;
  for (int i = 0; i < 4; i++) {
    offsets[i] = total;
    total += FFALIGN(sizes[i], kTransferAlign);
  }

  if (!transferPool_ || transferPoolSize_ != total) {
    av_buffer_pool_uninit(&transferPool_); // frees once in-flight frames are released
    transferPool_ = av_buffer_pool_init2(total, nullptr, allocTransferBuffer, nullptr);
    transferPoolSize_ = total;
    if (!transferPool_)
      return false;
  }
  out->buf[0] = av_buffer_pool_get(transferPool_);
  if (!out->buf[0])
    return false;
  out->format = fmt;
  out->width = hw->width;
  out->height = hw->height;
  for (int i = 0; i < 4 && sizes[i]; i++) {
    out->data[i] = out->buf[0]->data + offsets[i];
    out->linesize[i] = linesizes[i];
  }
  return true;
}
#endif

VideoDecoder::VideoDecoder() = default;

VideoDecoder::~VideoDecoder() { close(); }

AVPixelFormat VideoDecoder::getFormat(AVCodecContext *ctx, const AVPixelFormat *fmts) {
  auto *self = static_cast<VideoDecoder *>(ctx->opaque);
  for (const AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
    if (*p == self->hwPixFmt_)
      return *p;
  brls::Logger::warning(
    "VideoDecoder: hardware pixel format not offered, falling back to software"
  );
  self->hwPixFmt_ = AV_PIX_FMT_NONE;
  return fmts[0];
}

bool VideoDecoder::tryHardware(const AVCodec *codec) {
  // Preferred hardware device types per platform; any of them works with the
  // generic hwaccel + av_hwframe_transfer_data path below.
  const char *candidates[] = {
#ifdef __SWITCH__
    "nvtegra",
#elif defined(__APPLE__)
    "videotoolbox",
#elif defined(_WIN32)
    "d3d11va",
    "dxva2",
#else
    "vaapi",
    "vdpau",
#endif
  };
  for (const char *name : candidates) {
    AVHWDeviceType type = av_hwdevice_find_type_by_name(name);
    if (type == AV_HWDEVICE_TYPE_NONE)
      continue;
    AVPixelFormat pixFmt = AV_PIX_FMT_NONE;
    for (int i = 0;; i++) {
      const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
      if (!cfg)
        break;
      if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == type) {
        pixFmt = cfg->pix_fmt;
        break;
      }
    }
    if (pixFmt == AV_PIX_FMT_NONE)
      continue;
    AVBufferRef *device = nullptr;
    int rc = av_hwdevice_ctx_create(&device, type, nullptr, nullptr, 0);
    if (rc < 0) {
      char err[128];
      av_strerror(rc, err, sizeof err);
      brls::Logger::warning("VideoDecoder: {} unavailable: {}", name, err);
      continue;
    }
    hwDeviceCtx_ = device;
    hwPixFmt_ = pixFmt;
    codecCtx_->hw_device_ctx = av_buffer_ref(device);
    codecCtx_->get_format = &VideoDecoder::getFormat;
    stats_.codecName = codec->name;
    stats_.hwAccelName = name;
    stats_.hardware = true;
    brls::Logger::info("VideoDecoder: using hardware decoder {}", name);
    return true;
  }
  return false;
}

bool VideoDecoder::open() {
  close();
  const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (!codec) {
    brls::Logger::error("VideoDecoder: no H.264 decoder in this FFmpeg build");
    return false;
  }
  codecCtx_ = avcodec_alloc_context3(codec);
  if (!codecCtx_)
    return false;
  codecCtx_->opaque = this;
  codecCtx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
  codecCtx_->flags2 |= AV_CODEC_FLAG2_FAST;
  codecCtx_->err_recognition = 0;
  codecCtx_->skip_frame = AVDISCARD_DEFAULT;

  const char *swOnly = std::getenv("VKPCNX_SW_DECODE");
  if ((swOnly && swOnly[0] == '1') || !tryHardware(codec)) {
    codecCtx_->thread_type = FF_THREAD_SLICE; // frame threading adds latency
    codecCtx_->thread_count = 4;
    stats_.codecName = codec->name;
    stats_.hwAccelName.reset();
    stats_.hardware = false;
    brls::Logger::info("VideoDecoder: using software decoder {}", codec->name);
  }

  int rc = avcodec_open2(codecCtx_, codec, nullptr);
  if (rc < 0) {
    char err[128];
    av_strerror(rc, err, sizeof err);
    brls::Logger::error("VideoDecoder: avcodec_open2 failed: {}", err);
    close();
    return false;
  }
  packet_ = av_packet_alloc();
  frame_ = av_frame_alloc();
  running_ = true;
  thread_ = std::thread([this] {
    vkpcnx::diag::setThreadName("decoder");
    run();
  });
  return true;
}

void VideoDecoder::close() {
  running_ = false;
  cv_.notify_all();
  if (thread_.joinable())
    thread_.join();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    latest_.reset();
    latestIsNew_ = false;
  }
  if (packet_)
    av_packet_free(&packet_);
  if (frame_)
    av_frame_free(&frame_);
  if (codecCtx_)
    avcodec_free_context(&codecCtx_);
  if (hwDeviceCtx_)
    av_buffer_unref(&hwDeviceCtx_);
#ifdef __SWITCH__
  av_buffer_pool_uninit(&transferPool_);
  transferPoolSize_ = 0;
#endif
  hwPixFmt_ = AV_PIX_FMT_NONE;
}

void VideoDecoder::push(const uint8_t *data, size_t size, uint32_t rtpTimestamp) {
  if (!running_)
    return;
  if (const char *dump = std::getenv("VKPCNX_DUMP_H264")) {
    // Debug aid: append every access unit to an Annex B file
    if (FILE *f = std::fopen(dump, "ab")) {
      std::fwrite(data, 1, size, f);
      std::fclose(f);
    }
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= MAX_QUEUED_PACKETS) {
      // Dropping any single packet would corrupt every frame after it, so
      // drop the whole backlog and resume from the next keyframe
      stats_.framesDropped += queue_.size();
      queue_.clear();
      resync_ = true;
    }
    queue_.push_back({std::vector<uint8_t>(data, data + size), rtpTimestamp});
  }
  cv_.notify_one();
}

void VideoDecoder::resync() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    resync_ = true;
  }
  cv_.notify_one();
}

FramePtr VideoDecoder::takeFrame() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!latestIsNew_)
    return nullptr;
  latestIsNew_ = false;
  return latest_;
}

VideoDecoder::Stats VideoDecoder::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Stats s = stats_;
  s.avgDecodeMs = decodeMsCount_ ? decodeMsAccum_ / decodeMsCount_ : 0;
  return s;
}

// True if the Annex B access unit starts a keyframe (IDR slice or SPS)
static bool isKeyframe(const std::vector<uint8_t> &au) {
  for (size_t i = 0; i + 3 < au.size(); i++) {
    if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
      uint8_t type = au[i + 3] & 0x1f;
      if (type == 5 || type == 7)
        return true;
      i += 2;
    }
  }
  return false;
}

void VideoDecoder::waitForKeyframe() {
  auto now = std::chrono::steady_clock::now();
  if (!waitingForKeyframe_) {
    waitingForKeyframe_ = true;
    waitSince_ = now;
  }
  if (now - lastKeyframeRequest_ >= KEYFRAME_REQUEST_INTERVAL) {
    lastKeyframeRequest_ = now;
    if (onDecodeError)
      onDecodeError();
  }
}

void VideoDecoder::logWindow() {
  auto now = std::chrono::steady_clock::now();
  if (now - window_.since < std::chrono::seconds(5))
    return;
  Stats s = stats();
  uint64_t decoded = s.framesDecoded - window_.decoded;
  uint64_t dropped = s.framesDropped - window_.dropped;
  uint64_t errors = s.decodeErrors - window_.errors;
  if (dropped || errors || window_.skipped)
    brls::Logger::warning(
      "VideoDecoder: last 5 s: {} frames, avg decode {:.1f} ms, {} packets dropped (queue "
      "full), {} decode errors, {} packets skipped waiting for a keyframe",
      decoded,
      s.avgDecodeMs,
      dropped,
      errors,
      window_.skipped
    );
  window_ = {now, s.framesDecoded, s.framesDropped, s.decodeErrors, 0};
}

void VideoDecoder::run() {
  window_ = {std::chrono::steady_clock::now(), 0, 0, 0, 0};
  while (running_) {
    Packet packet;
    bool resync;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return !running_ || !queue_.empty(); });
      if (!running_)
        break;
      packet = std::move(queue_.front());
      queue_.pop_front();
      resync = resync_;
      resync_ = false;
    }
    if (resync)
      waitForKeyframe();

    if (waitingForKeyframe_) {
      if (isKeyframe(packet.data)) {
        waitingForKeyframe_ = false;
        avcodec_flush_buffers(codecCtx_); // drop references to broken frames
      } else if (std::chrono::steady_clock::now() - waitSince_ < KEYFRAME_WAIT_LIMIT) {
        window_.skipped++;
        waitForKeyframe(); // re-request periodically
        logWindow();
        continue;
      } else {
        // No keyframe is coming (e.g. intra-refresh stream); decode anyway
        waitingForKeyframe_ = false;
      }
    }

    if (!decodePacket(packet)) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.decodeErrors++;
      }
      waitForKeyframe();
    }
    logWindow();
  }
}

bool VideoDecoder::decodePacket(const Packet &packet) {
  using Clock = std::chrono::steady_clock;
  auto start = Clock::now();

  // AVPacket wants padded, refcounted data
  std::vector<uint8_t> *owned = new std::vector<uint8_t>(packet.data);
  owned->resize(owned->size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
  packet_->buf = av_buffer_create(
    owned->data(),
    static_cast<int>(owned->size()),
    [](void *opaque, uint8_t *) { delete static_cast<std::vector<uint8_t> *>(opaque); },
    owned,
    0
  );
  packet_->data = owned->data();
  packet_->size = static_cast<int>(packet.data.size());
  packet_->pts = packet.rtpTimestamp;

  int rc = avcodec_send_packet(codecCtx_, packet_);
  av_packet_unref(packet_);
  if (rc < 0 && rc != AVERROR(EAGAIN)) {
    char err[128];
    av_strerror(rc, err, sizeof err);
    brls::Logger::debug("VideoDecoder: send_packet: {}", err);
    return false;
  }

  bool got = false;
  while (true) {
    rc = avcodec_receive_frame(codecCtx_, frame_);
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
      break;
    if (rc < 0) {
      char err[128];
      av_strerror(rc, err, sizeof err);
      brls::Logger::debug("VideoDecoder: receive_frame: {}", err);
      return false;
    }

    AVFrame *out = av_frame_alloc();
    if (hwPixFmt_ != AV_PIX_FMT_NONE && frame_->format == hwPixFmt_) {
#ifdef __SWITCH__
      if (!allocTransferFrame(out, frame_))
        av_frame_unref(out); // let the transfer allocate (CPU copy fallback)
#endif
      rc = av_hwframe_transfer_data(out, frame_, 0);
      if (rc < 0) {
        char err[128];
        av_strerror(rc, err, sizeof err);
        brls::Logger::warning("VideoDecoder: hwframe transfer: {}", err);
        av_frame_free(&out);
        av_frame_unref(frame_);
        continue;
      }
      av_frame_copy_props(out, frame_);
    } else {
      av_frame_move_ref(out, frame_);
    }
    av_frame_unref(frame_);

    FramePtr fp(out, [](AVFrame *f) { av_frame_free(&f); });
    std::lock_guard<std::mutex> lock(mutex_);
    latest_ = fp;
    latestIsNew_ = true;
    stats_.framesDecoded++;
    stats_.width = out->width;
    stats_.height = out->height;
    got = true;
  }

  if (got) {
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    std::lock_guard<std::mutex> lock(mutex_);
    decodeMsAccum_ += ms;
    decodeMsCount_++;
    if (decodeMsCount_ > 600) { // rolling-ish average
      decodeMsAccum_ /= 2;
      decodeMsCount_ /= 2;
    }
  }
  return true;
}

} // namespace vkpcnx::media
