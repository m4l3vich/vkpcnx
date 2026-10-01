#include "core/media/audio_decoder.hpp"

#include <borealis/core/logger.hpp>
#include <opus.h>

#include "core/media/audio_output.hpp"

namespace vkpcnx::media {

static constexpr int MAX_FRAME_SIZE = 5760; // 120 ms at 48 kHz

AudioDecoder::AudioDecoder() = default;

AudioDecoder::~AudioDecoder() { close(); }

bool AudioDecoder::open() {
  close();
  int err = 0;
  decoder_ = opus_decoder_create(AudioOutput::SAMPLE_RATE, AudioOutput::CHANNELS, &err);
  if (err != OPUS_OK || !decoder_) {
    brls::Logger::error("AudioDecoder: opus_decoder_create failed: {}", opus_strerror(err));
    decoder_ = nullptr;
    return false;
  }
  return true;
}

void AudioDecoder::close() {
  if (decoder_) {
    opus_decoder_destroy(decoder_);
    decoder_ = nullptr;
  }
}

size_t AudioDecoder::decode(const uint8_t *packet, size_t size, std::vector<int16_t> &pcm) {
  if (!decoder_)
    return 0;
  pcm.resize(MAX_FRAME_SIZE * AudioOutput::CHANNELS);
  int frames =
    opus_decode(decoder_, packet, static_cast<opus_int32>(size), pcm.data(), MAX_FRAME_SIZE, 0);
  if (frames < 0) {
    brls::Logger::debug("AudioDecoder: opus_decode: {}", opus_strerror(frames));
    pcm.clear();
    return 0;
  }
  pcm.resize(static_cast<size_t>(frames) * AudioOutput::CHANNELS);
  return static_cast<size_t>(frames);
}

size_t AudioDecoder::decodeLost(std::vector<int16_t> &pcm) {
  if (!decoder_)
    return 0;
  pcm.resize(MAX_FRAME_SIZE * AudioOutput::CHANNELS);
  int frames = opus_decode(decoder_, nullptr, 0, pcm.data(), 960, 0);
  if (frames < 0) {
    pcm.clear();
    return 0;
  }
  pcm.resize(static_cast<size_t>(frames) * AudioOutput::CHANNELS);
  return static_cast<size_t>(frames);
}

} // namespace vkpcnx::media
