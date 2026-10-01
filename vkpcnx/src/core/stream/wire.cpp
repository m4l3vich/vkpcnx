#include "core/stream/wire.hpp"

#include <cstring>

namespace vkpcnx::stream {

static void putU32(std::vector<uint8_t> &out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 24));
}

static uint32_t getU32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

std::vector<uint8_t> encodeFrame(uint32_t type, const std::string &payload) {
  std::vector<uint8_t> out;
  out.reserve(FRAME_HEADER_SIZE + payload.size());
  putU32(out, static_cast<uint32_t>(4 + payload.size()));
  putU32(out, type);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

std::optional<Frame> decodeFrame(const uint8_t *data, size_t size) {
  if (size < FRAME_HEADER_SIZE)
    return std::nullopt;
  uint32_t length = getU32(data);
  if (length < 4 || static_cast<size_t>(length) + 4 != size)
    return std::nullopt;
  Frame f;
  f.type = getU32(data + 4);
  f.payload.assign(reinterpret_cast<const char *>(data + FRAME_HEADER_SIZE), length - 4);
  return f;
}

std::string messageTypeName(uint32_t type) {
  const auto *desc = cg::network::protocol::MessageType_descriptor();
  if (const auto *v = desc->FindValueByNumber(static_cast<int>(type)))
    return v->name();
  return std::to_string(type);
}

} // namespace vkpcnx::stream
