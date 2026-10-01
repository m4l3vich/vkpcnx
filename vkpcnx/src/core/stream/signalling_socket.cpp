#include "core/stream/signalling_socket.hpp"

#include <borealis.hpp>

#include "core/utils/tls.hpp"

namespace vkpcnx::stream {

using cg::network::protocol::MessageType;

SignallingSocket::SignallingSocket(std::string name) : name_(std::move(name)) {
  ws_.onOpen = [this] {
    brls::Logger::info("{}: connected to {}", name_, url_);
    keepAlive_.start(std::chrono::milliseconds(KEEP_ALIVE_INTERVAL_MS), [this] {
      // PeriodicTimer thread; WebSocket::sendBinary is thread-safe
      ws_.sendBinary(encodeMessage(MessageType::M_KEEP_ALIVE));
    });
    if (onOpen)
      onOpen();
  };
  ws_.onBinary = [this](const std::vector<uint8_t> &data) { handleBinary(data); };
  ws_.onMessage = [this](const std::string &text) {
    brls::Logger::warning("{}: ignoring unexpected text frame ({} bytes)", name_, text.size());
  };
  ws_.onError = [this](const std::string &error) {
    brls::Logger::error("{}: {}", name_, error);
    if (onError && !closing_)
      onError(error);
  };
  ws_.onClose = [this](int code, const std::string &reason) {
    keepAlive_.stop();
    bool wasClosing = closing_;
    brls::Logger::info("{}: closed ({} {})", name_, code, reason);
    finishClose();
    if (onClose)
      onClose(code, reason, code != 1006 || wasClosing);
  };
}

SignallingSocket::~SignallingSocket() {
  *alive_ = false;
  if (beforeCloseTimeout_)
    brls::cancelDelay(beforeCloseTimeout_);
  keepAlive_.stop();
}

void SignallingSocket::connect(const std::string &url) {
  url_ = url;
  beforeCloseOutstanding_ = false;
  closing_ = false;
  WebSocket::Options opts;
  opts.connectTimeoutSecs = 15;
  opts.pingIntervalSecs = 0; // the protocol has its own keep-alive
  opts.caFilePath = vkpcnx::utils::caBundlePath();
  opts.verifyTls = (!opts.caFilePath.empty() || vkpcnx::utils::hasSystemCaStore()) &&
                   !vkpcnx::utils::allowInsecureTls();
  if (!opts.verifyTls)
    brls::Logger::warning("{}: no CA bundle available, TLS verification disabled", name_);
  brls::Logger::info("{}: connecting to {}", name_, url);
  ws_.connect(url, opts);
}

void SignallingSocket::send(MessageType type, const std::string &payload) {
  sendRaw(encodeFrame(static_cast<uint32_t>(type), payload));
}

void SignallingSocket::sendRaw(const std::vector<uint8_t> &bytes) {
  if (!ws_.isOpen()) {
    brls::Logger::warning("{}: dropping message while socket is not open", name_);
    return;
  }
  ws_.sendBinary(bytes);
}

void SignallingSocket::close(int code, const std::string &reason) {
  closing_ = true;
  keepAlive_.stop();
  ws_.close(code, reason);
}

void SignallingSocket::gracefulClose(std::function<void()> done) {
  if (closeDone_) {
    // Already closing; chain the callbacks
    auto prev = std::move(closeDone_);
    closeDone_ = [prev, done] {
      if (prev)
        prev();
      if (done)
        done();
    };
    return;
  }
  closeDone_ = std::move(done);

  if (!ws_.isOpen()) {
    if (ws_.state() == WebSocket::State::Closed) {
      finishClose();
      return;
    }
    close();
    return;
  }

  closing_ = true;
  beforeCloseOutstanding_ = true;
  send(MessageType::M_CLIENT_BEFORE_CLOSE);
  std::weak_ptr<bool> alive = alive_;
  beforeCloseTimeout_ = brls::delay(BEFORE_CLOSE_TIMEOUT_MS, [this, alive] {
    if (auto a = alive.lock(); !a || !*a)
      return;
    beforeCloseTimeout_ = 0;
    if (beforeCloseOutstanding_) {
      brls::Logger::warning("{}: no M_CLIENT_BEFORE_CLOSE_CONFIRMATION, closing anyway", name_);
      beforeCloseOutstanding_ = false;
      close();
    }
  });
}

void SignallingSocket::finishClose() {
  if (beforeCloseTimeout_) {
    brls::cancelDelay(beforeCloseTimeout_);
    beforeCloseTimeout_ = 0;
  }
  beforeCloseOutstanding_ = false;
  closing_ = false;
  if (closeDone_) {
    auto cb = std::move(closeDone_);
    closeDone_ = nullptr;
    cb();
  }
}

void SignallingSocket::handleBinary(const std::vector<uint8_t> &data) {
  auto frame = decodeFrame(data);
  if (!frame) {
    brls::Logger::warning("{}: malformed frame ({} bytes)", name_, data.size());
    send(MessageType::M_MESSAGE_NOT_CORRECT);
    return;
  }

  switch (frame->type) {
  case MessageType::M_KEEP_ALIVE:
    return;
  case MessageType::M_MESSAGE_NOT_CORRECT:
    brls::Logger::error("{}: server reports M_MESSAGE_NOT_CORRECT", name_);
    return;
  case MessageType::M_UNSUPPORTED: {
    cg::network::protocol::Unsupported msg;
    msg.ParseFromString(frame->payload);
    brls::Logger::warning(
      "{}: server doesn't support {}", name_, messageTypeName(msg.messag_type())
    );
    return;
  }
  case MessageType::M_CLIENT_BEFORE_CLOSE_CONFIRMATION:
    if (beforeCloseOutstanding_) {
      beforeCloseOutstanding_ = false;
      close();
    }
    return;
  default:
    break;
  }

  // §4: everything else is ignored while a before-close is outstanding
  if (beforeCloseOutstanding_ || closing_) {
    brls::Logger::debug("{}: ignoring {} while closing", name_, messageTypeName(frame->type));
    return;
  }

  if (!cg::network::protocol::MessageType_IsValid(static_cast<int>(frame->type))) {
    brls::Logger::warning("{}: unknown message type {}", name_, frame->type);
    cg::network::protocol::Unsupported reply;
    reply.set_messag_type(static_cast<int32_t>(frame->type));
    send(MessageType::M_UNSUPPORTED, reply);
    return;
  }

  brls::Logger::debug(
    "{}: <- {} ({} bytes)", name_, messageTypeName(frame->type), frame->payload.size()
  );
  if (onFrame)
    onFrame(*frame);
}

} // namespace vkpcnx::stream
