#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// Minimal libwebsockets-based WebSocket client.
//
// Each instance owns a background thread that drives the libwebsockets event
// loop for the lifetime of one connection. All public methods are thread-safe
// and never block on network I/O. Callbacks are delivered on the UI thread
// (via brls::sync), so it's safe to touch views from them; a callback still
// queued when the WebSocket is destroyed is dropped.
class WebSocket {
public:
  using Headers = std::map<std::string, std::string>;

  enum class State { Closed, Connecting, Open, Closing };

  struct Options {
    Headers headers;         // extra handshake headers (e.g. Authorization)
    std::string subprotocol; // Sec-WebSocket-Protocol, empty for none
    std::string origin;      // Origin header value, defaults to http(s)://host[:port]
    std::string caFilePath;  // CA bundle; required for wss:// under mbedTLS (Switch)
    bool verifyTls = true;   // false accepts self-signed/expired certs and hostname mismatch
    int connectTimeoutSecs = 10;
    int pingIntervalSecs = 30; // idle seconds before a PING is sent, 0 disables keepalive
    int pingTimeoutSecs = 10;  // seconds without a PONG after a PING before hanging up
    // Invoke callbacks directly on the service thread instead of posting them
    // to the UI thread. Lower latency (used for RTT measurement); the callbacks
    // must then be thread-safe and must not touch views.
    bool directCallbacks = false;
  };

  WebSocket();
  ~WebSocket();
  WebSocket(const WebSocket &) = delete;
  WebSocket &operator=(const WebSocket &) = delete;

  // Callbacks run on the UI thread (unless Options::directCallbacks). Browser semantics: onError (if any) is
  // always followed by onClose; code 1006 means the connection dropped
  // without a close handshake.
  std::function<void()> onOpen;
  std::function<void(const std::string &text)> onMessage;
  std::function<void(const std::vector<uint8_t> &data)> onBinary;
  std::function<void(int code, const std::string &reason)> onClose;
  std::function<void(const std::string &error)> onError;

  // Starts connecting to a ws:// or wss:// URL. Ignored unless state() is
  // Closed. Outcome is reported through onOpen or onError/onClose.
  void connect(const std::string &url, const Options &options);
  void connect(const std::string &url) { connect(url, Options()); }

  // Queue a message; dropped (with a warning) unless the socket is Open.
  void send(const std::string &text);
  void sendBinary(const std::vector<uint8_t> &data);
  void sendJson(const nlohmann::json &json);

  // Starts a clean close handshake; onClose fires once it completes.
  void close(int code = 1000, const std::string &reason = "");

  State state() const;
  bool isOpen() const { return state() == State::Open; }

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
