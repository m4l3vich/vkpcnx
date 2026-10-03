#include "core/websocket.hpp"
#include "core/diag/log.hpp"
#include "core/utils/thread.hpp"

#include <algorithm>
#include <chrono>
#include <borealis.hpp>
#include <cstring>
#include <deque>
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#include <libwebsockets.h>
#include <mutex>
#include <thread>

// ---- lws logging -----------------------------------------------------------

static void lwsLog(int level, const char *line) {
  std::string msg = line ? line : "";
  while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
    msg.pop_back();
  if (level & LLL_ERR)
    brls::Logger::error("lws: {}", msg);
  else if (level & LLL_WARN)
    brls::Logger::warning("lws: {}", msg);
  else
    brls::Logger::verbose("lws: {}", msg);
}

static void initLwsLogging() {
  static std::once_flag once;
  std::call_once(once, [] {
    int levels = LLL_ERR | LLL_WARN;
    // Connection-stage tracing, the only way to see where a handshake stalls
    // on the Switch; logged at verbose level
    if (brls::Logger::getLogLevel() >= brls::LogLevel::LOG_VERBOSE)
      levels |= LLL_NOTICE | LLL_INFO | LLL_CLIENT;
    lws_set_log_level(levels, lwsLog);
  });
}

// Every address `host` resolves to, in getaddrinfo order. lws 4.3 treats a
// refused/reset connect (POLLHUP) as a dead socket and gives up instead of
// moving on to the next A record, so the fallback is done here instead.
static std::vector<std::string> resolveAddresses(const std::string &host, int port) {
  std::vector<std::string> out;
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *res = nullptr;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) == 0) {
    for (addrinfo *ai = res; ai; ai = ai->ai_next) {
      // inet_ntop rather than getnameinfo(NI_NUMERICHOST): libnx's getnameinfo
      // ignores the flag and returns the reverse-DNS name, which may not resolve
      char buf[INET6_ADDRSTRLEN];
      const void *addr = nullptr;
      if (ai->ai_family == AF_INET)
        addr = &reinterpret_cast<const sockaddr_in *>(ai->ai_addr)->sin_addr;
#ifdef AF_INET6
      else if (ai->ai_family == AF_INET6)
        addr = &reinterpret_cast<const sockaddr_in6 *>(ai->ai_addr)->sin6_addr;
#endif
      if (addr && inet_ntop(ai->ai_family, addr, buf, sizeof buf) &&
          std::find(out.begin(), out.end(), buf) == out.end())
        out.emplace_back(buf);
    }
    freeaddrinfo(res);
  }
  if (out.empty())
    out.push_back(host); // let lws resolve it and report the failure
  return out;
}

// ---- impl ------------------------------------------------------------------

struct WebSocket::Impl : std::enable_shared_from_this<Impl> {
  struct Message {
    std::vector<uint8_t> data;
    bool binary;
  };

  // Shared between the caller's thread and the service thread
  mutable std::mutex mutex;
  WebSocket *owner;           // nulled by ~WebSocket; callbacks are read through it
  lws_context *ctx = nullptr; // non-null only while the service thread is running
  State state = State::Closed;
  bool stop = false; // ask the service loop to exit
  std::deque<Message> txQueue;
  bool closeRequested = false;
  int closeCode = 1006;
  std::string closeReason;

  // Owned by whoever calls connect()/~WebSocket
  std::thread thread;

  // Service thread only
  lws *wsi = nullptr;
  Options opts;
  std::string host, hostHeader, path, origin;
  int port = 0;
  bool ssl = false;
  bool established = false;
  std::vector<std::string> addresses; // what `host` resolves to, tried in order
  size_t addressIndex = 0;
  bool retryPending = false; // the current address failed; the loop should try the next
  lws_retry_bo_t retry{};
  std::vector<uint8_t> rxBuffer;
  bool rxBinary = false;
  std::vector<uint8_t> txBuffer;
  bool finished = false; // onClose already delivered for this connection

  explicit Impl(WebSocket *owner) : owner(owner) {}

  void run();
  int callback(lws *wsi, lws_callback_reasons reason, void *in, size_t len);
  void finish(int code, const std::string &reason, const std::string &error = {});
  void wake();
  // Detaches this connection from its WebSocket (no more callbacks) and asks
  // the service loop to exit
  void release();
  // Waits for the service thread, logging a slow stop
  void joinService();

  // Runs `fn(WebSocket&)` on the UI thread, unless the WebSocket is gone by then
  template <class F> void post(F fn) {
    if (opts.directCallbacks) {
      WebSocket *ws;
      {
        std::lock_guard<std::mutex> lock(mutex);
        ws = owner;
      }
      if (ws)
        fn(*ws);
      return;
    }
    std::weak_ptr<Impl> weak = weak_from_this();
    brls::sync([weak, fn]() {
      auto self = weak.lock();
      if (!self)
        return;
      WebSocket *ws;
      {
        std::lock_guard<std::mutex> lock(self->mutex);
        ws = self->owner;
      }
      if (ws)
        fn(*ws);
    });
  }

  static int lwsCallback(lws *wsi, lws_callback_reasons reason, void *user, void *in, size_t len) {
    auto *self = static_cast<Impl *>(lws_context_user(lws_get_context(wsi)));
    return self ? self->callback(wsi, reason, in, len) : 0;
  }

  static const lws_protocols kProtocols[];
};

const lws_protocols WebSocket::Impl::kProtocols[] = {
  {"vkpcnx-ws", &WebSocket::Impl::lwsCallback, 0, 0, 0, nullptr, 0},
  LWS_PROTOCOL_LIST_TERM,
};

void WebSocket::Impl::wake() {
  std::lock_guard<std::mutex> lock(mutex);
  if (ctx)
    lws_cancel_service(ctx);
}

void WebSocket::Impl::finish(int code, const std::string &reason, const std::string &error) {
  if (finished)
    return;
  finished = true;
  wsi = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex);
    state = State::Closed;
    stop = true;
    txQueue.clear();
  }
  if (!error.empty()) {
    brls::Logger::error("WebSocket: {}", error);
    post([error](WebSocket &ws) {
      auto cb = ws.onError;
      if (cb)
        cb(error);
    });
  } else {
    brls::Logger::debug("WebSocket: closed ({} {})", code, reason);
  }
  post([code, reason](WebSocket &ws) {
    auto cb = ws.onClose;
    if (cb)
      cb(code, reason);
  });
}

int WebSocket::Impl::callback(lws *wsi, lws_callback_reasons reason, void *in, size_t len) {
  switch (reason) {
  case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
    auto **p = static_cast<unsigned char **>(in);
    unsigned char *end = *p + len;
    for (auto &[name, value] : opts.headers) {
      std::string key = name + ":";
      if (lws_add_http_header_by_name(
            wsi,
            reinterpret_cast<const unsigned char *>(key.c_str()),
            reinterpret_cast<const unsigned char *>(value.c_str()),
            static_cast<int>(value.size()),
            p,
            end
          )) {
        brls::Logger::error("WebSocket: handshake header buffer too small");
        return -1;
      }
    }
    return 0;
  }

  case LWS_CALLBACK_CLIENT_ESTABLISHED: {
    established = true;
    {
      std::lock_guard<std::mutex> lock(mutex);
      // close() may have raced the handshake; keep Closing so it proceeds below
      if (state == State::Connecting)
        state = State::Open;
    }
    brls::Logger::debug("WebSocket: connected to {}", host);
    post([](WebSocket &ws) {
      auto cb = ws.onOpen;
      if (cb)
        cb();
    });
    return 0;
  }

  case LWS_CALLBACK_CLIENT_RECEIVE: {
    if (lws_is_first_fragment(wsi)) {
      rxBuffer.clear();
      rxBinary = lws_frame_is_binary(wsi);
    }
    auto *bytes = static_cast<const uint8_t *>(in);
    rxBuffer.insert(rxBuffer.end(), bytes, bytes + len);
    if (lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0) {
      if (rxBinary) {
        post([data = rxBuffer](WebSocket &ws) {
          auto cb = ws.onBinary;
          if (cb)
            cb(data);
        });
      } else {
        post([text = std::string(rxBuffer.begin(), rxBuffer.end())](WebSocket &ws) {
          auto cb = ws.onMessage;
          if (cb)
            cb(text);
        });
      }
      rxBuffer.clear();
    }
    return 0;
  }

  case LWS_CALLBACK_EVENT_WAIT_CANCELLED: {
    // send()/close() were called from another thread; act on the service thread
    std::lock_guard<std::mutex> lock(mutex);
    if (!this->wsi)
      return 0;
    if (closeRequested && !established)
      lws_set_timeout(this->wsi, PENDING_TIMEOUT_USER_OK, LWS_TO_KILL_ASYNC); // abort handshake
    else if (!txQueue.empty() || closeRequested)
      lws_callback_on_writable(this->wsi);
    return 0;
  }

  case LWS_CALLBACK_CLIENT_WRITEABLE: {
    Message msg;
    bool more;
    {
      std::lock_guard<std::mutex> lock(mutex);
      // Messages queued before close() still go out first (e.g. M_BYE)
      if (closeRequested && txQueue.empty()) {
        // Returning non-zero closes the connection with the reason set here
        lws_close_reason(
          wsi,
          static_cast<lws_close_status>(closeCode),
          reinterpret_cast<unsigned char *>(closeReason.data()),
          std::min<size_t>(closeReason.size(), 123)
        );
        return -1;
      }
      if (txQueue.empty())
        return 0;
      msg = std::move(txQueue.front());
      txQueue.pop_front();
      more = !txQueue.empty() || closeRequested;
    }

    txBuffer.resize(LWS_PRE + msg.data.size());
    std::memcpy(txBuffer.data() + LWS_PRE, msg.data.data(), msg.data.size());
    int n = lws_write(
      wsi,
      txBuffer.data() + LWS_PRE,
      msg.data.size(),
      msg.binary ? LWS_WRITE_BINARY : LWS_WRITE_TEXT
    );
    if (n < static_cast<int>(msg.data.size())) {
      brls::Logger::error("WebSocket: write failed ({} of {} bytes)", n, msg.data.size());
      return -1;
    }
    if (more)
      lws_callback_on_writable(wsi);
    return 0;
  }

  case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE: {
    std::lock_guard<std::mutex> lock(mutex);
    auto *bytes = static_cast<const uint8_t *>(in);
    if (bytes && len >= 2) {
      closeCode = (bytes[0] << 8) | bytes[1];
      closeReason.assign(reinterpret_cast<const char *>(bytes + 2), len - 2);
    }
    return 0;
  }

  case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
    bool aborted;
    int code;
    std::string reason;
    {
      std::lock_guard<std::mutex> lock(mutex);
      aborted = closeRequested;
      code = closeCode;
      reason = closeReason;
    }
    if (aborted) {
      finish(code, reason); // close() during the handshake, not an error
    } else {
      std::string err = in ? static_cast<const char *>(in) : "connection failed";
      if (addressIndex + 1 < addresses.size()) {
        brls::Logger::warning(
          "WebSocket: {} via {} failed ({}), trying the next address",
          host,
          addresses[addressIndex],
          err
        );
        addressIndex++;
        retryPending = true;
        this->wsi = nullptr;
        return 0;
      }
      finish(1006, err, err);
    }
    return 0;
  }

  case LWS_CALLBACK_CLIENT_CLOSED: {
    int code;
    std::string reason;
    {
      std::lock_guard<std::mutex> lock(mutex);
      code = closeCode;
      reason = closeReason;
    }
    finish(code, reason);
    return 0;
  }

  default:
    return 0;
  }
}

void WebSocket::Impl::run() {
  lws_context_creation_info info{};
  info.port = CONTEXT_PORT_NO_LISTEN;
  info.protocols = kProtocols;
  info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT | LWS_SERVER_OPTION_JUST_USE_RAW_ORIGIN;
  info.gid = -1;
  info.uid = -1;
  info.user = this;
  info.fd_limit_per_thread = 4;
  info.connect_timeout_secs = opts.connectTimeoutSecs;
  info.timeout_secs = opts.connectTimeoutSecs;
  if (!opts.caFilePath.empty())
    info.client_ssl_ca_filepath = opts.caFilePath.c_str();

  lws_context *context = lws_create_context(&info);
  if (!context) {
    finish(1006, "lws_create_context failed", "lws_create_context failed");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex);
    ctx = context;
  }

  // Only the idle/keepalive part of the policy is used; reconnecting is up to the caller
  retry = {};
  retry.secs_since_valid_ping = static_cast<uint16_t>(opts.pingIntervalSecs);
  retry.secs_since_valid_hangup =
    static_cast<uint16_t>(opts.pingIntervalSecs + opts.pingTimeoutSecs);

  // Resolved after lws_create_context: on Windows that is what runs WSAStartup
  addresses = resolveAddresses(host, port);
  addressIndex = 0;
  retryPending = false;

  lws_client_connect_info ci{};
  ci.context = context;
  ci.port = port;
  ci.path = path.c_str();
  ci.host = hostHeader.c_str(); // also what SNI and the cert hostname check use
  ci.origin = origin.c_str();
  ci.protocol = opts.subprotocol.empty() ? nullptr : opts.subprotocol.c_str();
  ci.local_protocol_name = kProtocols[0].name;
  ci.pwsi = &wsi;
  ci.retry_and_idle_policy = opts.pingIntervalSecs > 0 ? &retry : nullptr;
  if (ssl) {
    ci.ssl_connection = LCCSCF_USE_SSL;
    if (!opts.verifyTls)
      ci.ssl_connection |= LCCSCF_ALLOW_SELFSIGNED | LCCSCF_ALLOW_EXPIRED | LCCSCF_ALLOW_INSECURE |
                           LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
  }

  auto attempt = [&] {
    wsi = nullptr;
    ci.address = addresses[addressIndex].c_str();
    brls::Logger::debug(
      "WebSocket: connecting to {}://{}:{}{} via {} ({}/{})",
      ssl ? "wss" : "ws",
      host,
      port,
      path,
      addresses[addressIndex],
      addressIndex + 1,
      addresses.size()
    );
    // A null return means CLIENT_CONNECTION_ERROR already ran: it either
    // queued the next address or reported the failure
    if (!lws_client_connect_via_info(&ci) && !retryPending)
      finish(1006, "connection failed", "connection failed");

    // send()/close() calls that raced this attempt found no wsi to act on
    std::lock_guard<std::mutex> lock(mutex);
    if (!txQueue.empty() || closeRequested)
      lws_cancel_service(context);
  };

  attempt();
  while (true) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (stop)
        break;
    }
    // Drain synchronous failures here: with no wsi left, lws_service would
    // block with nothing to wake it
    while (retryPending && !finished) {
      retryPending = false;
      attempt();
    }
    if (finished)
      break;
    if (lws_service(context, 0) < 0)
      break;
  }

  {
    std::lock_guard<std::mutex> lock(mutex);
    ctx = nullptr;
  }
  lws_context_destroy(context); // delivers CLIENT_CLOSED for a still-open connection
  finish(1006, "service loop ended");
}

// ---- public API ------------------------------------------------------------

WebSocket::WebSocket() : impl_(std::make_shared<Impl>(this)) {}

void WebSocket::Impl::release() {
  std::lock_guard<std::mutex> lock(mutex);
  owner = nullptr;
  stop = true;
  if (ctx)
    lws_cancel_service(ctx);
}

void WebSocket::Impl::joinService() {
  if (!thread.joinable())
    return;
  // The service loop sees `stop` as soon as lws_cancel_service() wakes it;
  // a slow stop means the wake-up was lost (lws then only wakes on its own
  // timers, every 30 s when idle)
  auto started = std::chrono::steady_clock::now();
  thread.join();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - started
  )
              .count();
  if (ms >= 1000)
    brls::Logger::warning("WebSocket: {} took {} ms to stop", host, ms);
}

WebSocket::~WebSocket() {
  impl_->release();
  impl_->joinService();
}

void WebSocket::abandon() {
  std::shared_ptr<Impl> old = std::move(impl_);
  impl_ = std::make_shared<Impl>(this);
  old->release();
  if (old->thread.joinable())
    vkpcnx::utils::runDetached([old] { old->joinService(); });
}

void WebSocket::connect(const std::string &url, const Options &options) {
  Impl &impl = *impl_;
  {
    std::lock_guard<std::mutex> lock(impl.mutex);
    if (impl.state != State::Closed) {
      brls::Logger::warning("WebSocket: connect() ignored, socket is not closed");
      return;
    }
    impl.state = State::Connecting;
  }
  // Reap the previous connection's thread; it exits right after setting Closed
  if (impl.thread.joinable())
    impl.thread.join();

  // lws_parse_uri() splits in place and drops the leading '/' of the path
  std::string buf = url;
  const char *prot = nullptr, *ads = nullptr, *pathPart = nullptr;
  int port = 0;
  if (lws_parse_uri(buf.data(), &prot, &ads, &port, &pathPart) || !ads || !*ads ||
      (strcmp(prot, "ws") != 0 && strcmp(prot, "wss") != 0)) {
    {
      std::lock_guard<std::mutex> lock(impl.mutex);
      impl.state = State::Closed;
    }
    std::string err = "invalid WebSocket URL: " + url;
    brls::Logger::error("WebSocket: {}", err);
    impl.post([err](WebSocket &ws) {
      auto cb = ws.onError;
      if (cb)
        cb(err);
    });
    impl.post([err](WebSocket &ws) {
      auto cb = ws.onClose;
      if (cb)
        cb(1006, err);
    });
    return;
  }

  impl.opts = options;
  impl.ssl = strcmp(prot, "wss") == 0;
  impl.host = ads;
  impl.port = port;
  impl.hostHeader = impl.host;
  if (port != (impl.ssl ? 443 : 80))
    impl.hostHeader += ":" + std::to_string(port);
  impl.path = std::string("/") + (pathPart && strcmp(pathPart, "/") != 0 ? pathPart : "");
  impl.origin =
    options.origin.empty() ? (impl.ssl ? "https://" : "http://") + impl.hostHeader : options.origin;
  impl.wsi = nullptr;
  impl.established = false;
  impl.rxBuffer.clear();
  impl.finished = false;
  {
    std::lock_guard<std::mutex> lock(impl.mutex);
    impl.stop = false;
    impl.txQueue.clear();
    impl.closeRequested = false;
    impl.closeCode = 1006;
    impl.closeReason.clear();
  }

  initLwsLogging();
  impl.thread = std::thread([impl = impl_] {
    vkpcnx::diag::setThreadName("ws");
    impl->run();
  });
}

void WebSocket::send(const std::string &text) {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->state != State::Open) {
      brls::Logger::warning("WebSocket: send() dropped, socket is not open");
      return;
    }
    impl_->txQueue.push_back({std::vector<uint8_t>(text.begin(), text.end()), false});
  }
  impl_->wake();
}

void WebSocket::sendBinary(const std::vector<uint8_t> &data) {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->state != State::Open) {
      brls::Logger::warning("WebSocket: sendBinary() dropped, socket is not open");
      return;
    }
    impl_->txQueue.push_back({data, true});
  }
  impl_->wake();
}

void WebSocket::sendJson(const nlohmann::json &json) { send(json.dump()); }

void WebSocket::close(int code, const std::string &reason) {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->state != State::Open && impl_->state != State::Connecting)
      return;
    impl_->state = State::Closing;
    impl_->closeRequested = true;
    impl_->closeCode = code;
    impl_->closeReason = reason;
  }
  impl_->wake();
}

WebSocket::State WebSocket::state() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->state;
}
