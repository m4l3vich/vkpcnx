#pragma once
#include <borealis.hpp>
#include <string>
#include <webp/demux.h>

namespace vkpcnx {
// Plays an animated WebP (a still WebP works too, as a one-frame animation).
//
// Inherits brls::Image so scalingType / imageAlign / interpolation and the
// FIT/FILL layout behave exactly like a regular image. Frames are decoded
// lazily from the compressed data as they become due, so memory stays at the
// file size plus two RGBA canvases regardless of the frame count; the cost is
// one frame decode on the render thread each time the animation advances.
//
// XML attributes (on top of Image's):
//   webp="@res/img/spinner.webp"   file to play
//   loop="true|false"              default true (honours the file's loop count if set)
//   autoplay="true|false"          default true
class AnimatedWebp : public brls::Image {
public:
  AnimatedWebp();
  ~AnimatedWebp() override;

  void setWebpFromRes(const std::string &name);
  void setWebpFromFile(const std::string &path);
  void setWebpFromMem(const unsigned char *data, size_t size);
  void setWebpFromMem(std::string data);
  // Same contract as brls::Image::setImageAsync: cb receives a setter to call
  // (from any thread) once the bytes are available.
  void setWebpAsync(std::function<void(std::function<void(const std::string &)>)> cb);
  void setWebpFromUrl(const std::string &url);

  void play();
  void pause();
  // Rewinds to the first frame; keeps playing if it was playing.
  void restart();
  bool isPlaying() const { return this->playing; }

  void setLooping(bool looping) { this->looping = looping; }
  bool isLooping() const { return this->looping; }
  void setAutoplay(bool autoplay) { this->autoplay = autoplay; }

  int getFrameCount() const { return this->info.frame_count; }

  void clear();

  void draw(
    NVGcontext *vg,
    float x,
    float y,
    float width,
    float height,
    brls::Style style,
    brls::FrameContext *ctx
  ) override;

  static brls::View *create();

private:
  // Owns the compressed bytes for as long as the decoder lives (libwebp keeps
  // pointing into them).
  std::string data;
  WebPAnimDecoder *decoder = nullptr;
  WebPAnimInfo info {};

  bool looping = true;
  bool autoplay = true;
  bool playing = false;

  // Presentation timeline. libwebp reports each frame's *end* timestamp (ms
  // since animation start), so a frame is due for replacement once
  // now - frameShownAt >= (timestamp - previous timestamp).
  int prevTimestampMs = 0;
  brls::Time currentFrameDurationUs = 0;
  brls::Time frameShownAt = 0;
  int loopsDone = 0;

  void destroyDecoder();
  // Decodes the next frame into the texture. Returns false at the end of a
  // non-looping animation or on decode error.
  bool advanceFrame(NVGcontext *vg);
};
} // namespace vkpcnx
