#include "core/media/audio_output.hpp"
#include "core/diag/log.hpp"

#include <algorithm>
#include <borealis/core/logger.hpp>
#include <cstring>

#ifdef __SWITCH__
#include <switch.h>
#include <thread>
#else
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
#endif

namespace vkpcnx::media {

#ifdef __SWITCH__

// audout: 48 kHz stereo int16, buffers of 5 ms appended from a worker thread
struct AudioOutput::Backend {
  static constexpr size_t BUFFER_FRAMES = 240; // 5 ms
  static constexpr int BUFFER_COUNT = 4;

  AudioOutput *owner = nullptr;
  std::thread thread;
  std::atomic<bool> running{false};
  AudioOutBuffer buffers[BUFFER_COUNT]{};
  void *memory[BUFFER_COUNT]{};

  bool start(AudioOutput *o) {
    owner = o;
    Result rc = audoutInitialize();
    if (R_FAILED(rc)) {
      brls::Logger::error("AudioOutput: audoutInitialize failed: {:#x}", rc);
      return false;
    }
    rc = audoutStartAudioOut();
    if (R_FAILED(rc)) {
      brls::Logger::error("AudioOutput: audoutStartAudioOut failed: {:#x}", rc);
      audoutExit();
      return false;
    }
    size_t bytes = BUFFER_FRAMES * CHANNELS * sizeof(int16_t);
    size_t aligned = (bytes + 0xfff) & ~0xfff;
    for (int i = 0; i < BUFFER_COUNT; i++) {
      memory[i] = aligned_alloc(0x1000, aligned);
      std::memset(memory[i], 0, aligned);
      buffers[i].next = nullptr;
      buffers[i].buffer = memory[i];
      buffers[i].buffer_size = aligned;
      buffers[i].data_size = bytes;
      buffers[i].data_offset = 0;
      audoutAppendAudioOutBuffer(&buffers[i]);
    }
    running = true;
    thread = std::thread([this] {
      vkpcnx::diag::setThreadName("audio");
      run();
    });
    return true;
  }

  void run() {
    while (running) {
      AudioOutBuffer *released = nullptr;
      u32 count = 0;
      if (R_FAILED(audoutWaitPlayFinish(&released, &count, 100000000ULL)) || !released)
        continue;
      owner->read(static_cast<int16_t *>(released->buffer), BUFFER_FRAMES);
      audoutAppendAudioOutBuffer(released);
    }
  }

  void stop() {
    running = false;
    if (thread.joinable())
      thread.join();
    audoutStopAudioOut();
    audoutExit();
    for (auto &m : memory) {
      free(m);
      m = nullptr;
    }
  }
};

#else

struct AudioOutput::Backend {
  ma_device device{};
  bool started = false;

  static void callback(ma_device *device, void *out, const void *, ma_uint32 frames) {
    auto *owner = static_cast<AudioOutput *>(device->pUserData);
    owner->read(static_cast<int16_t *>(out), frames);
  }

  bool start(AudioOutput *owner) {
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_s16;
    config.playback.channels = CHANNELS;
    config.sampleRate = SAMPLE_RATE;
    config.dataCallback = callback;
    config.pUserData = owner;
    config.periodSizeInMilliseconds = 10;
    if (ma_device_init(nullptr, &config, &device) != MA_SUCCESS) {
      brls::Logger::error("AudioOutput: ma_device_init failed");
      return false;
    }
    if (ma_device_start(&device) != MA_SUCCESS) {
      brls::Logger::error("AudioOutput: ma_device_start failed");
      ma_device_uninit(&device);
      return false;
    }
    started = true;
    return true;
  }

  void stop() {
    if (started) {
      ma_device_uninit(&device);
      started = false;
    }
  }
};

#endif

AudioOutput::AudioOutput() : backend_(std::make_unique<Backend>()) {
  ring_.assign(MAX_BUFFERED_FRAMES * CHANNELS, 0);
}

AudioOutput::~AudioOutput() { close(); }

bool AudioOutput::open() {
  if (open_)
    return true;
  open_ = backend_->start(this);
  return open_;
}

void AudioOutput::close() {
  if (!open_)
    return;
  backend_->stop();
  open_ = false;
  std::lock_guard<std::mutex> lock(mutex_);
  readPos_ = 0;
  count_ = 0;
}

size_t AudioOutput::bufferedFrames() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return count_ / CHANNELS;
}

void AudioOutput::write(const int16_t *samples, size_t frames) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t n = frames * CHANNELS;
  size_t cap = ring_.size();
  if (n > cap) {
    samples += n - cap;
    n = cap;
  }
  if (count_ + n > cap) {
    // drop the oldest to make room
    size_t drop = count_ + n - cap;
    readPos_ = (readPos_ + drop) % cap;
    count_ -= drop;
    overruns_++;
  }
  size_t writePos = (readPos_ + count_) % cap;
  size_t first = std::min(n, cap - writePos);
  std::memcpy(&ring_[writePos], samples, first * sizeof(int16_t));
  if (n > first)
    std::memcpy(&ring_[0], samples + first, (n - first) * sizeof(int16_t));
  count_ += n;
}

void AudioOutput::read(int16_t *out, size_t frames) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t n = frames * CHANNELS;
  size_t cap = ring_.size();
  size_t avail = std::min(n, count_);
  size_t first = std::min(avail, cap - readPos_);
  std::memcpy(out, &ring_[readPos_], first * sizeof(int16_t));
  if (avail > first)
    std::memcpy(out + first, &ring_[0], (avail - first) * sizeof(int16_t));
  readPos_ = (readPos_ + avail) % cap;
  count_ -= avail;
  if (avail < n) {
    std::memset(out + avail, 0, (n - avail) * sizeof(int16_t));
    if (avail == 0)
      underruns_++;
  }
}

} // namespace vkpcnx::media
