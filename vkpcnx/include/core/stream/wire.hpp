#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "protocol_common.pb.h"

// Wire framing shared by the manager/game-server WebSockets and the input
// DataChannel (protocol doc §3):
//
//   u32 LE length  = 4 + payload size
//   u32 LE type    = cg.network.protocol.MessageType
//   bytes payload  = protobuf (may be empty)
//
// One WebSocket/DataChannel message carries exactly one frame.
namespace vkpcnx::stream {

using MessageType = cg::network::protocol::MessageType;

struct Frame {
  uint32_t type = 0;
  std::string payload;
};

constexpr size_t FRAME_HEADER_SIZE = 8;

std::vector<uint8_t> encodeFrame(uint32_t type, const std::string &payload = {});

template <class Message> std::vector<uint8_t> encodeMessage(MessageType type, const Message &msg) {
  return encodeFrame(static_cast<uint32_t>(type), msg.SerializeAsString());
}

inline std::vector<uint8_t> encodeMessage(MessageType type) {
  return encodeFrame(static_cast<uint32_t>(type));
}

// Returns nullopt on a short buffer or a length field that doesn't match.
std::optional<Frame> decodeFrame(const uint8_t *data, size_t size);
inline std::optional<Frame> decodeFrame(const std::vector<uint8_t> &data) {
  return decodeFrame(data.data(), data.size());
}

// Enum name for logs ("M_KEEP_ALIVE"), or the number if unknown
std::string messageTypeName(uint32_t type);

} // namespace vkpcnx::stream
