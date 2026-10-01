#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}

namespace vkpcnx::media {

using FramePtr = std::shared_ptr<AVFrame>;

// H.264 decoder on a worker thread. Access units come in from the WebRTC
// thread (push()), decoded frames are picked up by the renderer with
// takeFrame(); only the newest frame is kept so display latency stays at one
// frame. Uses a hardware decoder when available (nvtegra on Switch,
// VideoToolbox on macOS) and copies hardware frames to system memory.
class VideoDecoder {
public:
  static constexpr size_t MAX_QUEUED_PACKETS = 4;
  // After a decode error or dropped packets, skip to the next keyframe, asking
  // for one this often, and give up waiting after the limit
  static constexpr std::chrono::milliseconds KEYFRAME_REQUEST_INTERVAL{1000};
  static constexpr std::chrono::milliseconds KEYFRAME_WAIT_LIMIT{2000};

  VideoDecoder();
  ~VideoDecoder();

  // Called from the decoder thread when it needs a keyframe (after a decode
  // error or a queue overflow), rate-limited
  std::function<void()> onDecodeError;

  bool open();
  void close();
  bool isOpen() const { return codecCtx_ != nullptr; }

  // Annex B access unit; copies the data
  void push(const uint8_t *data, size_t size, uint32_t rtpTimestamp);

  // Input was lost: skip to the next keyframe (requesting one). Thread-safe.
  void resync();

  // Newest decoded frame (system-memory pixel format), or null if none is new
  FramePtr takeFrame();

  struct Stats {
    uint64_t framesDecoded = 0;
    uint64_t framesDropped = 0; // packets dropped from the queue
    uint64_t decodeErrors = 0;
    double avgDecodeMs = 0;
    int width = 0, height = 0;
    std::string codecName;                  // e.g. "h264"
    std::optional<std::string> hwAccelName; // e.g. "videotoolbox"; nullopt for software
    bool hardware = false;
  };
  Stats stats() const;

private:
  struct Packet {
    std::vector<uint8_t> data;
    uint32_t rtpTimestamp;
  };

  void run();
  bool decodePacket(const Packet &packet);
  void waitForKeyframe();
  void logWindow();
  bool tryHardware(const AVCodec *codec);
  static AVPixelFormat getFormat(AVCodecContext *ctx, const AVPixelFormat *fmts);
#ifdef __SWITCH__
  bool allocTransferFrame(AVFrame *out, const AVFrame *hw);
  AVBufferPool *transferPool_ = nullptr;
  size_t transferPoolSize_ = 0;
#endif

  AVCodecContext *codecCtx_ = nullptr;
  AVBufferRef *hwDeviceCtx_ = nullptr;
  AVPixelFormat hwPixFmt_ = AV_PIX_FMT_NONE;
  AVPacket *packet_ = nullptr;
  AVFrame *frame_ = nullptr;

  std::thread thread_;
  std::atomic<bool> running_{false};
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Packet> queue_;
  FramePtr latest_;
  bool latestIsNew_ = false;
  bool resync_ = false; // set by push() on queue overflow
  Stats stats_;

  // Decoder thread only
  bool waitingForKeyframe_ = false;
  std::chrono::steady_clock::time_point waitSince_, lastKeyframeRequest_;
  struct Window {
    std::chrono::steady_clock::time_point since;
    uint64_t decoded, dropped, errors, skipped;
  } window_{};
  double decodeMsAccum_ = 0;
  uint64_t decodeMsCount_ = 0;
};

} // namespace vkpcnx::media
