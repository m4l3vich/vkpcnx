#include "core/stream/game_server_client.hpp"

#include <borealis.hpp>

namespace vkpcnx::stream {

namespace proto = cg::network::protocol;
using proto::MessageType;

GameServerClient::GameServerClient() : socket_("GameServer") {
  socket_.onOpen = [this] {
    bool reconnect = everConnected_;
    everConnected_ = true;
    authenticated_ = false;
    sendAuth();
    if (reconnect) {
      if (onReconnected)
        onReconnected();
    } else if (onConnected) {
      onConnected();
    }
  };
  socket_.onFrame = [this](const Frame &frame) { handleFrame(frame); };
  socket_.onError = [this](const std::string &error) {
    if (!everConnected_ && reconnectAttempt_ == 0 && onConnectFailed)
      onConnectFailed(error);
  };
  socket_.onClose = [this](int code, const std::string &reason, bool clean) {
    if (closing_) {
      if (onClosed)
        onClosed();
      return;
    }
    if (!everConnected_ && reconnectAttempt_ == 0) {
      // Initial connection failed; onConnectFailed already reported it
      if (onClosed)
        onClosed();
      return;
    }
    if (clean) {
      brls::Logger::info("GameServer: closed cleanly by server ({} {})", code, reason);
      if (onClosed)
        onClosed();
      return;
    }
    scheduleReconnect();
  };
}

GameServerClient::~GameServerClient() {
  *alive_ = false;
  if (reconnectDelay_)
    brls::cancelDelay(reconnectDelay_);
}

void GameServerClient::connect(const GameServerAddress &server) {
  server_ = server;
  everConnected_ = false;
  authenticated_ = false;
  needReconnection_ = false;
  closing_ = false;
  reconnectAttempt_ = 0;
  socket_.connect(server.url());
}

void GameServerClient::scheduleReconnect() {
  // §5.4: first silently, second with a modal, third → error
  reconnectAttempt_++;
  if (reconnectAttempt_ >= MAX_RECONNECT_ATTEMPTS) {
    SessionError e;
    e.kind = SessionError::Kind::RemoteHostNotResponding;
    e.message = "Remote host is not responding";
    if (onError)
      onError(e);
    if (onClosed)
      onClosed();
    return;
  }
  brls::Logger::warning(
    "GameServer: connection dropped, reconnecting (attempt {})", reconnectAttempt_
  );
  if (onReconnecting)
    onReconnecting(reconnectAttempt_);
  std::weak_ptr<bool> alive = alive_;
  reconnectDelay_ = brls::delay(500, [this, alive] {
    if (auto a = alive.lock(); !a || !*a)
      return;
    reconnectDelay_ = 0;
    if (!closing_)
      socket_.connect(server_.url());
  });
}

void GameServerClient::sendAuth() {
  proto::cs::Auth auth;
  auth.set_id_session(server_.sessionId);
  auth.set_id_user(server_.userId);
  auth.set_server_name(server_.name);
  auth.set_token_session(server_.tokenSession);
  auth.set_id_connect(server_.connectId);
  socket_.send(MessageType::CS_AUTH, auth);
}

void GameServerClient::sendSdpOffer(StreamType type, const std::string &sdp, bool reconfigurate) {
  proto::cs::SdpOffer offer;
  offer.set_type(type);
  offer.set_sdp(sdp);
  offer.set_reconfigurate_flag(reconfigurate);
  offer.set_server_ip(server_.ip);
  brls::Logger::info(
    "GameServer: -> CS_SDP_OFFER type={} reconfigurate={} ({} bytes)",
    static_cast<int>(type),
    reconfigurate,
    sdp.size()
  );
  socket_.send(MessageType::CS_SDP_OFFER, offer);
}

void GameServerClient::sendIceCandidate(StreamType type, const std::string &iceJson) {
  proto::IceCandidate c;
  c.set_type(type);
  c.set_ice(iceJson);
  socket_.send(MessageType::CS_ICE_CANDIDATE, c);
}

void GameServerClient::sendStreamsStatus(uint32_t flags) {
  proto::cs::StreamsStatus s;
  s.set_flags(flags);
  brls::Logger::info("GameServer: -> CS_STREAMS_STATUS flags={}", flags);
  socket_.send(MessageType::CS_STREAMS_STATUS, s);
}

void GameServerClient::gracefulClose(std::function<void()> done) {
  closing_ = true;
  if (reconnectDelay_) {
    brls::cancelDelay(reconnectDelay_);
    reconnectDelay_ = 0;
  }
  socket_.gracefulClose(std::move(done));
}

void GameServerClient::close() {
  closing_ = true;
  if (reconnectDelay_) {
    brls::cancelDelay(reconnectDelay_);
    reconnectDelay_ = 0;
  }
  socket_.close(1000, "BYE");
}

void GameServerClient::handleFrame(const Frame &frame) {
  switch (frame.type) {
  case MessageType::M_ACCEPT:
    break; // no-op on this socket (§5.1)

  case MessageType::SC_AUTH_SUCCESS: {
    authenticated_ = true;
    reconnectAttempt_ = 0;
    proto::Capabilities caps;
    caps.set_steam_bigpicture(false);
    socket_.send(MessageType::M_CAPABILITIES, caps);
    break;
  }

  case MessageType::M_CAPABILITIES: {
    proto::Capabilities caps;
    caps.ParseFromString(frame.payload);
    brls::Logger::info(
      "GameServer: server capabilities (modern_clipboard={})", caps.modern_clipboard()
    );
    if (needReconnection_) {
      brls::Logger::info("GameServer: -> CS_RECONNECT");
      socket_.send(MessageType::CS_RECONNECT);
    } else if (onReadyForOffers) {
      onReadyForOffers();
    }
    break;
  }

  case MessageType::SC_SDP_ANSWER: {
    proto::sc::SdpAnswer a;
    a.ParseFromString(frame.payload);
    brls::Logger::info(
      "GameServer: <- SC_SDP_ANSWER type={} ({} bytes)", static_cast<int>(a.type()), a.sdp().size()
    );
    if (onSdpAnswer)
      onSdpAnswer(a.type(), a.sdp(), static_cast<uint32_t>(a.control_token()));
    break;
  }

  case MessageType::SC_ICE_CANDIDATE: {
    proto::IceCandidate c;
    c.ParseFromString(frame.payload);
    brls::Logger::debug("GameServer: <- SC_ICE_CANDIDATE {{ {} }}", c.ShortDebugString());
    if (onIceCandidate)
      onIceCandidate(c.type(), c.ice());
    break;
  }

  case MessageType::M_SET_LOG_LEVEL: {
    proto::SetLogLevel l;
    l.ParseFromString(frame.payload);
    logLevel_ = l.level();
    brls::Logger::info("GameServer: log level {} requested", logLevel_);
    break;
  }

  case MessageType::SC_EXIT_GAME:
    if (onSessionEnd)
      onSessionEnd(EndReason::ExitGame);
    break;
  case MessageType::SC_DEMO_PLAY_END:
    if (onSessionEnd)
      onSessionEnd(EndReason::DemoPlayEnd);
    break;
  case MessageType::SC_GAME_PLAY_END:
    if (onSessionEnd)
      onSessionEnd(EndReason::GamePlayEnd);
    break;
  case MessageType::SC_TRIAL_TIME_FINISHED:
    if (onSessionEnd)
      onSessionEnd(EndReason::TrialTimeFinished);
    break;
  case MessageType::SC_SUBSCRIPTION_LIMIT_TIME_EXPIRED:
    if (onSessionEnd)
      onSessionEnd(EndReason::SubscriptionLimitExpired);
    break;
  case MessageType::SC_SUBSCRIPTION_EXPIRED:
    if (onSessionEnd)
      onSessionEnd(EndReason::SubscriptionExpired);
    break;

  case MessageType::SC_INACTIVE_TIMEOUT: {
    SessionError e;
    e.kind = SessionError::Kind::SessionTimeout;
    e.message = "Session timed out due to inactivity";
    if (onError)
      onError(e);
    break;
  }
  case MessageType::SC_CHANGE_CLIENT: {
    SessionError e;
    e.kind = SessionError::Kind::DuplicateClient;
    e.message = "Another client took over this session";
    if (onError)
      onError(e);
    break;
  }
  case MessageType::SC_SUBSCRIPTION_PART_OF_DAY_EXPIRED: {
    proto::sc::SubscriptionPartOfDayEnd s;
    s.ParseFromString(frame.payload);
    brls::Logger::debug(
      "GameServer: <- SC_SUBSCRIPTION_PART_OF_DAY_EXPIRED {{ {} }}", s.ShortDebugString()
    );
    SessionError e;
    e.kind = SessionError::Kind::SubscriptionExpired;
    e.title = s.header();
    e.message = s.text();
    if (onError)
      onError(e);
    break;
  }

  case MessageType::SC_LEFT_DEMO_TIME: {
    proto::sc::LeftDemoTime t;
    t.ParseFromString(frame.payload);
    brls::Logger::debug("GameServer: <- SC_LEFT_DEMO_TIME {{ {} }}", t.ShortDebugString());
    demoTimeLeft_ = t.count_seconds();
    emitTimeLeft();
    break;
  }
  case MessageType::SC_LEFT_GAME_TIME: {
    proto::sc::LeftGameTime t;
    t.ParseFromString(frame.payload);
    brls::Logger::debug("GameServer: <- SC_LEFT_GAME_TIME {{ {} }}", t.ShortDebugString());
    gameTimeLeft_ = t.count_seconds();
    emitTimeLeft();
    break;
  }
  case MessageType::SC_SUBSCRIPTION_LIMIT_TIME_LEFT: {
    proto::sc::SubscriptionLimitTimeLeft t;
    t.ParseFromString(frame.payload);
    brls::Logger::debug(
      "GameServer: <- SC_SUBSCRIPTION_LIMIT_TIME_LEFT {{ {} }}", t.ShortDebugString()
    );
    subscriptionLimitLeft_ = t.time_left_seconds();
    emitTimeLeft();
    break;
  }
  case MessageType::SC_SUBSCRIPTION_PART_OF_DAY_LEFT: {
    proto::sc::SubscriptionPartOfDayLeft t;
    t.ParseFromString(frame.payload);
    brls::Logger::debug(
      "GameServer: <- SC_SUBSCRIPTION_PART_OF_DAY_LEFT {{ {} }}", t.ShortDebugString()
    );
    subscriptionDailyLeft_ = t.time_left_seconds();
    emitTimeLeft();
    break;
  }
  case MessageType::SC_GAME_DISABLE_TIMER:
    if (onTimerDisabled)
      onTimerDisabled();
    break;

  case MessageType::SC_DISCONNECT_STREAMS:
    brls::Logger::info("GameServer: <- SC_DISCONNECT_STREAMS");
    if (onDisconnectStreams)
      onDisconnectStreams();
    break;
  case MessageType::SC_RECONNECT_STREAMS:
    brls::Logger::info("GameServer: <- SC_RECONNECT_STREAMS");
    if (onReconnectStreams)
      onReconnectStreams();
    break;
  case MessageType::SC_VM_REBOOT: {
    proto::sc::VmReboot r;
    r.ParseFromString(frame.payload);
    brls::Logger::info("GameServer: <- SC_VM_REBOOT shutdown={}", r.is_shutdown());
    if (onVmReboot)
      onVmReboot(r.is_shutdown());
    break;
  }

  case MessageType::SC_SESSION_EVENT: {
    proto::SessionEvent ev;
    ev.ParseFromString(frame.payload);
    brls::Logger::info(
      "GameServer: session event {} type={} payload={}", ev.name(), ev.type(), ev.payload()
    );
    if (ev.type() == "session_end") {
      if (onSessionEnd)
        onSessionEnd(EndReason::SessionEvent);
    } else if (onSessionEvent) {
      onSessionEvent({ev.name(), ev.type(), ev.payload(), ev.icon()});
    }
    break;
  }

  case MessageType::SC_OPEN_URL_IN_BROWSER: {
    proto::sc::OpenUrlInBrowser u;
    u.ParseFromString(frame.payload);
    brls::Logger::debug("GameServer: <- SC_OPEN_URL_IN_BROWSER {{ {} }}", u.ShortDebugString());
    if (onOpenUrl)
      onOpenUrl(u.url());
    break;
  }

  case MessageType::SC_GAME_UPDATE_BEGIN: {
    proto::sc::GameUpdateBegin g;
    g.ParseFromString(frame.payload);
    brls::Logger::debug("GameServer: <- SC_GAME_UPDATE_BEGIN {{ {} }}", g.ShortDebugString());
    if (onStatus)
      onStatus(g.text());
    break;
  }

  case MessageType::SC_VM_NAME: {
    proto::sc::VmName v;
    v.ParseFromString(frame.payload);
    brls::Logger::info("GameServer: VM {}", v.name());
    if (onVmName)
      onVmName(v.name());
    break;
  }
  case MessageType::SC_GAME_SESSION_LAUNCHER: {
    proto::sc::GameSessionLauncher l;
    l.ParseFromString(frame.payload);
    brls::Logger::info("GameServer: launcher {}", static_cast<int>(l.launcher()));
    break;
  }
  case MessageType::SC_GAME_SESSION_EVENT: {
    proto::sc::GameSessionEvent e;
    e.ParseFromString(frame.payload);
    brls::Logger::info(
      "GameServer: game session event {} {}", static_cast<int>(e.event()), e.description()
    );
    break;
  }
  case MessageType::SC_GAME_WAS_LAUNCHED: {
    proto::sc::GameWasLaunched g;
    g.ParseFromString(frame.payload);
    brls::Logger::info("GameServer: game launched ({})", g.executable());
    break;
  }
  case MessageType::SC_PLAY:
    // Native-transport ports; unused by the WebRTC client (§10)
    break;

  case MessageType::M_ERROR: {
    proto::Error err;
    err.ParseFromString(frame.payload);
    auto e = errorFromProto(err);
    brls::Logger::error(
      "GameServer: M_ERROR {} ({}): {}", static_cast<int>(e.kind), e.code, e.message
    );
    if (onError)
      onError(e);
    break;
  }
  case MessageType::M_ERROR_INFO: {
    proto::ErrorInfo info;
    info.ParseFromString(frame.payload);
    brls::Logger::error(
      "GameServer: M_ERROR_INFO component={} code={} {}",
      static_cast<int>(info.component_type()),
      info.error_code(),
      info.error_message()
    );
    break;
  }
  case MessageType::M_NOTIFICATION: {
    proto::Notification n;
    n.ParseFromString(frame.payload);
    brls::Logger::debug("GameServer: <- M_NOTIFICATION {{ {} }}", n.ShortDebugString());
    if (onNotification)
      onNotification(n.message());
    break;
  }
  case MessageType::M_BYE: {
    proto::Bye bye;
    bye.ParseFromString(frame.payload);
    brls::Logger::info("GameServer: M_BYE ({}, error={})", bye.reason(), bye.has_error());
    break;
  }

  default:
    brls::Logger::debug("GameServer: unhandled {}", messageTypeName(frame.type));
    break;
  }
}

void GameServerClient::emitTimeLeft() {
  std::optional<int64_t> best;
  for (auto v : {demoTimeLeft_, gameTimeLeft_, subscriptionLimitLeft_, subscriptionDailyLeft_}) {
    if (v && (!best || *v < *best))
      best = v;
  }
  if (best && onTimeLeft)
    onTimeLeft(*best);
}

} // namespace vkpcnx::stream
