#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace vkpcnx::media {

// Low-latency PCM output (interleaved int16). Desktop: miniaudio; Switch:
// libnx audout. Samples are pushed into a ring buffer from the decoder
// thread and pulled by the backend; when the buffer runs dry silence is
// played, when it overflows the oldest samples are dropped.
class AudioOutput {
public:
  static constexpr int SAMPLE_RATE = 48000;
  static constexpr int CHANNELS = 2;
  // Keep at most ~120 ms queued so latency can't build up
  static constexpr size_t MAX_BUFFERED_FRAMES = SAMPLE_RATE * 120 / 1000;

  AudioOutput();
  ~AudioOutput();

  bool open();
  void close();
  bool isOpen() const { return open_; }

  void write(const int16_t *samples, size_t frames);
  size_t bufferedFrames() const;

  // Backend pulls `frames` frames (zero-filled if not enough data)
  void read(int16_t *out, size_t frames);

private:
  struct Backend;
  std::unique_ptr<Backend> backend_;
  bool open_ = false;

  mutable std::mutex mutex_;
  std::vector<int16_t> ring_; // interleaved
  size_t readPos_ = 0;
  size_t count_ = 0; // samples (not frames) currently stored
  std::atomic<uint64_t> underruns_{0};
  std::atomic<uint64_t> overruns_{0};
};

} // namespace vkpcnx::media
