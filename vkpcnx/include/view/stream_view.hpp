#pragma once

#include <borealis.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>

#include "core/media/video_decoder.hpp"
#include "core/stream/input_channel.hpp"

namespace vkpcnx {

// Draws decoded video frames with OpenGL (YUV → RGB in a shader), letterboxed
// into the view's rectangle, plus the client-side cursor (§7.6). Frames are
// pulled from the VideoDecoder on the UI thread during draw().
class StreamView : public brls::View {
public:
  StreamView();
  ~StreamView() override;

  void draw(
    NVGcontext *vg,
    float x,
    float y,
    float width,
    float height,
    brls::Style style,
    brls::FrameContext *ctx
  ) override;

  void setDecoder(media::VideoDecoder *decoder) { decoder_ = decoder; }

  // Rendered video rectangle in window (content) coordinates; changes are
  // reported so CS_MOUSE_SETTINGS can follow (§7.3)
  brls::Rect videoRect() const { return videoRect_; }
  std::function<void(int width, int height)> onVideoRectChanged;

  // Client-side cursor. Position is in video-rect pixels. A default arrow is
  // shown until the server replaces it; an empty image hides the cursor.
  void setCursor(const stream::CursorImage &image);
  void setCursorVisible(bool visible) { cursorVisible_ = visible; }
  void setCursorPosition(float x, float y) {
    cursorX_ = x;
    cursorY_ = y;
  }

  bool hasFrame() const { return frameWidth_ > 0; }
  double renderedFps() const { return renderedFps_; }
  int frameWidth() const { return frameWidth_; }
  int frameHeight() const { return frameHeight_; }
  // Time when the first frame was drawn (video "started", §5.2)
  bool firstFrameDrawn() const { return firstFrame_; }
  std::function<void()> onFirstFrame;
  // Called at the start of every draw() (once per frame on the UI thread)
  std::function<void()> onDraw;
  // Drawn over the letterbox fill and the video, under the cursor, with the
  // video rectangle (the controls playground's desktop)
  std::function<void(NVGcontext *vg, const brls::Rect &rect)> onDrawBackdrop;

private:
  struct Gl;
  Gl *gl_ = nullptr;
  bool initGl();
  void destroyGl();
  void upload(const AVFrame *frame);
  void drawVideo(float x, float y, float width, float height);

  media::VideoDecoder *decoder_ = nullptr;
  int frameWidth_ = 0, frameHeight_ = 0;
  int texFormat_ = -1; // AVPixelFormat currently allocated
  int colorspace_ = 0, colorRange_ = 0;
  brls::Rect videoRect_;
  int reportedW_ = 0, reportedH_ = 0;
  bool firstFrame_ = false;

  // fps
  std::chrono::steady_clock::time_point fpsWindowStart_;
  int fpsCounter_ = 0;
  double renderedFps_ = 0;

  // cursor
  int cursorImage_ = 0; // nvg image handle
  int cursorW_ = 0, cursorH_ = 0, cursorHotX_ = 0, cursorHotY_ = 0;
  bool cursorVisible_ = false;
  float cursorX_ = 0, cursorY_ = 0;
  std::mutex cursorMutex_;
  stream::CursorImage pendingCursor_;
  bool cursorPending_ = false;
};

} // namespace vkpcnx
