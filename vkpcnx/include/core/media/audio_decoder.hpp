#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct OpusDecoder;

namespace vkpcnx::media {

// Opus → 48 kHz stereo int16
class AudioDecoder {
public:
  AudioDecoder();
  ~AudioDecoder();

  bool open();
  void close();
  bool isOpen() const { return decoder_ != nullptr; }

  // Returns the number of frames written to `pcm` (interleaved stereo)
  size_t decode(const uint8_t *packet, size_t size, std::vector<int16_t> &pcm);
  // Packet loss concealment for one missing packet
  size_t decodeLost(std::vector<int16_t> &pcm);

private:
  OpusDecoder *decoder_ = nullptr;
};

} // namespace vkpcnx::media
