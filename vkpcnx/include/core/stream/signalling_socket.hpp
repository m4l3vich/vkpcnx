#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "core/stream/wire.hpp"
#include "core/utils/periodic_timer.hpp"
#include "core/websocket.hpp"

namespace vkpcnx::stream {

// A WebSocket speaking the [len][type][protobuf] framing of protocol doc §3,
// with the behaviour common to the manager and game-server connections
// (§4, §5): M_KEEP_ALIVE every 6 s, M_UNSUPPORTED replies, the
// M_CLIENT_BEFORE_CLOSE handshake and the "ignore everything while closing"
// rule. All callbacks run on the UI thread.
class SignallingSocket {
public:
  static constexpr int KEEP_ALIVE_INTERVAL_MS = 6000;
  static constexpr int BEFORE_CLOSE_TIMEOUT_MS = 2000;

  explicit SignallingSocket(std::string name);
  ~SignallingSocket();

  std::function<void()> onOpen;
  // Frames not consumed internally (keep-alive, unsupported, before-close confirmation)
  std::function<void(const Frame &frame)> onFrame;
  // `clean` is false when the connection dropped without a close handshake (code 1006)
  std::function<void(int code, const std::string &reason, bool clean)> onClose;
  std::function<void(const std::string &error)> onError;

  void connect(const std::string &url);
  // Drops the current connection without a close handshake or waiting for it
  // (WebSocket::abandon: after a Switch sleep its socket is dead and its
  // thread can't be woken). No onClose for the dropped connection; connect()
  // can be called right away.
  void abandon();
  // abandon(), then connect to the same URL again
  void reconnect();

  void send(MessageType type, const std::string &payload = {});
  template <class Message> void send(MessageType type, const Message &msg) {
    sendRaw(encodeMessage(type, msg));
  }

  // Immediate close (1000 "BYE" by default)
  void close(int code = 1000, const std::string &reason = "BYE");

  // §5.3: M_CLIENT_BEFORE_CLOSE → wait up to 2 s for the confirmation → close.
  // `done` runs once the socket is closed (also when it wasn't open at all).
  void gracefulClose(std::function<void()> done);

  bool isOpen() const { return ws_.isOpen(); }
  WebSocket::State state() const { return ws_.state(); }
  const std::string &name() const { return name_; }
  const std::string &url() const { return url_; }

private:
  void sendRaw(const std::vector<uint8_t> &bytes);
  void handleBinary(const std::vector<uint8_t> &data);
  void finishClose();

  std::string name_;
  std::string url_;
  WebSocket ws_;
  vkpcnx::utils::PeriodicTimer keepAlive_;

  bool beforeCloseOutstanding_ = false;
  bool closing_ = false;
  size_t beforeCloseTimeout_ = 0;
  std::function<void()> closeDone_;
  // Guards callbacks queued for a destroyed socket
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace vkpcnx::stream
