#include "core/stream/manager_client.hpp"

#include <borealis.hpp>
#include <cmath>

namespace vkpcnx::stream {

namespace proto = cg::network::protocol;
using proto::MessageType;

static const int STANDARD_SIZES[][2] = {
  {1280, 720},
  {1366, 768},
  {1440, 900},
  {1600, 900},
  {1920, 1080},
  {2048, 1080},
  {2560, 1440},
  {3840, 2160},
  {4096, 2160},
};

std::pair<int, int> snapMonitorSize(int width, int height) {
  for (const auto &s : STANDARD_SIZES)
    if (std::abs(s[0] - width) <= 5 && std::abs(s[1] - height) <= 5)
      return {s[0], s[1]};
  return {width, height};
}

std::string gameServerHostFromDnsName(const std::string &dnsName) {
  size_t first = dnsName.find('.');
  if (first == std::string::npos)
    return dnsName;
  size_t second = dnsName.find('.', first + 1);
  std::string label =
    dnsName.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1);
  return label + ".clgrtc.ru";
}

SessionError errorFromProto(const proto::Error &err) {
  SessionError e;
  e.code = err.error_type();
  e.title = err.title_message();
  e.message = err.user_message().empty() ? err.error_message() : err.user_message();
  switch (err.error_type()) {
  case proto::MC_AUTHORIZATION_FAILED:
  case proto::MC_AUTHORIZATION_EXPIRED:
  case proto::SC_AUTH_BAD:
  case proto::SC_NOT_AUTH:
  case proto::SC_OLD_AUTH:
    e.kind = SessionError::Kind::BadAuthorization;
    break;
  case proto::MC_ALL_SERVERS_BUSY:
  case proto::MC_NO_SERVERS_FOUND:
  case proto::MC_NOT_EXISTS_ACTIVE_SERVERS:
  case proto::MC_VIP_SERVER_NOT_AVAILABLE:
    e.kind = SessionError::Kind::AllServersBusy;
    break;
  case proto::SC_AUTH_DUPLICATE:
  case proto::MC_AUTH_DUPLICATE:
    e.kind = SessionError::Kind::DuplicateClient;
    break;
  case proto::SC_CONTROLS_TIMEOUT:
    e.kind = SessionError::Kind::SessionTimeout;
    break;
  case proto::SC_LARGE_LOSSES_UDP:
    e.kind = SessionError::Kind::LowBandwidth;
    break;
  case proto::MC_TOO_FREQUENT_CONNECTION:
    e.kind = SessionError::Kind::TooFrequentConnection;
    break;
  case proto::MC_NEED_UPDATE:
  case proto::MC_NEED_UPDATE_GAME:
  case proto::SC_NEED_UPDATE_GAME:
    e.kind = SessionError::Kind::NeedUpdate;
    break;
  case proto::SC_SUBSCRIPTION_LIMIT_TIME_EXPIRED_ERROR:
    e.kind = SessionError::Kind::SubscriptionExpired;
    break;
  default:
    e.kind = SessionError::Kind::Generic;
    break;
  }
  return e;
}

ManagerClient::ManagerClient() : socket_("Manager") {
  zonePing_.onProgress = [this](int completed, int total) {
    if (onPingProgress)
      onPingProgress(completed, total);
  };
  socket_.onOpen = [this] {
    // §4: 30 s "manager didn't send play" watchdog, log only
    std::weak_ptr<bool> alive = alive_;
    playWatchdog_ = brls::delay(30000, [this, alive] {
      if (auto a = alive.lock(); !a || !*a)
        return;
      playWatchdog_ = 0;
      if (!gameServer_ && !finished_)
        brls::Logger::warning("Manager: no MC_DIRECTION_PLAY after 30 s");
    });
  };
  socket_.onFrame = [this](const Frame &frame) { handleFrame(frame); };
  socket_.onError = [this](const std::string &error) {
    SessionError e;
    e.kind = SessionError::Kind::ConnectionFailed;
    e.message = error;
    fail(e);
  };
  socket_.onClose = [this](int code, const std::string &reason, bool clean) {
    zonePing_.cancel();
    if (finished_)
      return;
    if (pingTestMode_) {
      // §4.2: the manager ends the ping test with M_BYE, which we already
      // handled; any other close is a failure
      finish(Result::Failed);
      return;
    }
    if (!gameServer_) {
      SessionError e;
      e.kind = clean ? SessionError::Kind::Generic : SessionError::Kind::ConnectionFailed;
      e.message = "Manager connection closed (" + std::to_string(code) + " " + reason + ")";
      fail(e);
    }
    // After MC_DIRECTION_PLAY the manager socket going away is expected
  };
}

ManagerClient::~ManagerClient() {
  *alive_ = false;
  if (playWatchdog_)
    brls::cancelDelay(playWatchdog_);
}

void ManagerClient::connect(const PlayUrl &playUrl, const Options &options) {
  playUrl_ = playUrl;
  options_ = options;
  pingTestMode_ = playUrl.workMode == PlayUrl::WorkMode::PingTest;
  finished_ = false;
  byeSent_ = false;
  gameServer_.reset();
  socket_.connect(playUrl.managerUrl());
}

void ManagerClient::fail(SessionError error) {
  if (finished_)
    return;
  brls::Logger::error(
    "Manager: error {} ({}): {}", static_cast<int>(error.kind), error.code, error.message
  );
  if (onError)
    onError(error);
  finish(Result::Failed);
}

void ManagerClient::finish(Result result) {
  if (finished_)
    return;
  finished_ = true;
  zonePing_.cancel();
  if (onFinished)
    onFinished(result);
}

void ManagerClient::close() {
  if (socket_.isOpen() && !byeSent_) {
    byeSent_ = true;
    proto::Bye bye;
    socket_.send(MessageType::M_BYE, bye);
  }
  socket_.close(1000, "BYE");
}

void ManagerClient::gracefulClose(std::function<void()> done) {
  socket_.gracefulClose(std::move(done));
}

void ManagerClient::sendHandshake() {
  if (pingTestMode_) {
    proto::cm::ValidateVersion vv;
    vv.set_software(CLIENT_SOFTWARE_NAME);
    vv.set_version(CLIENT_SOFTWARE_VERSION);
    socket_.send(MessageType::CM_VALIDATE_VERSION, vv);
    return;
  }

  proto::Capabilities caps;
  caps.add_capabilities("UDP_INPUTS");
  socket_.send(MessageType::M_CAPABILITIES, caps);

  proto::cm::SetLanguage lang;
  lang.set_language(playUrl_.language == "ru" ? proto::LANGUAGE_RU : proto::LANGUAGE_EN);
  socket_.send(MessageType::CM_SET_LANGUAGE, lang);

  auto [w, h] = snapMonitorSize(options_.monitorWidth, options_.monitorHeight);
  proto::SystemInfo info;
  auto *pw = info.add_system_params();
  pw->set_name("monitor-width-1");
  pw->set_value(std::to_string(w));
  auto *ph = info.add_system_params();
  ph->set_name("monitor-height-1");
  ph->set_value(std::to_string(h));
  socket_.send(MessageType::CM_SYSTEM_INFO, info);

  proto::VideoSettings vs;
  vs.set_max_fps(proto::VIDEO_FPS_AUTO);
  vs.set_max_resolution(proto::VIDEO_RESOLUTION_AUTO);
  if (options_.bitrateMinMbit > 0)
    vs.set_bitrate_min_kbps(options_.bitrateMinMbit * 1000);
  if (options_.bitrateMaxMbit > 0)
    vs.set_bitrate_max_kbps(options_.bitrateMaxMbit * 1000);
  if (options_.numberOfSlices > 0)
    vs.set_number_of_slices(options_.numberOfSlices);
  if (options_.numberOfRefFrames > 0)
    vs.set_number_of_ref_frames(options_.numberOfRefFrames);
  socket_.send(MessageType::CM_VIDEO_SETTINGS, vs);

  proto::cm::ClientPlatform platform;
  platform.set_name(options_.ios ? proto::cm::WEBRTCIOS : proto::cm::WEBRTC);
  socket_.send(MessageType::CM_CLIENT_PLATFORM, platform);

  proto::cm::ValidateVersion vv;
  vv.set_software(CLIENT_SOFTWARE_NAME);
  vv.set_version(CLIENT_SOFTWARE_VERSION);
  socket_.send(MessageType::CM_VALIDATE_VERSION, vv);
}

void ManagerClient::sendAuth() {
  if (pingTestMode_) {
    proto::cm::AuthToken auth;
    auth.set_token(playUrl_.token);
    auth.set_mode(proto::cm::AuthToken::PING_TEST);
    socket_.send(MessageType::CM_AUTH_TOKEN, auth);
    return;
  }

  if (!playUrl_.exeCmdLine.empty()) {
    proto::cm::ExecutableCmdLine cmd;
    cmd.set_exe_cmd_line("--exe-cmd-line=" + playUrl_.exeCmdLine);
    socket_.send(MessageType::CM_EXE_CMD_LINE, cmd);
  }
  if (!playUrl_.gcsettings.empty()) {
    proto::cm::ServerSettings settings;
    settings.set_gcsettings(playUrl_.gcsettings);
    socket_.send(MessageType::CM_SERVER_SETTINGS, settings);
  }

  switch (playUrl_.authType()) {
  case PlayUrl::AuthType::GameToken:
    // §2.1: token + gameId → nothing is sent, the manager already knows us
    break;
  case PlayUrl::AuthType::SingleToken: {
    proto::cm::AuthToken auth;
    auth.set_token(playUrl_.token);
    auth.set_mode(proto::cm::AuthToken::GAME_STREAM);
    socket_.send(MessageType::CM_AUTH_TOKEN, auth);
    break;
  }
  case PlayUrl::AuthType::UserPassword: {
    proto::cm::Auth auth;
    auth.set_login(playUrl_.username);
    auth.set_password(playUrl_.password);
    if (playUrl_.gameId)
      auth.set_game_id(playUrl_.gameId);
    socket_.send(MessageType::CM_AUTH, auth);
    break;
  }
  }
}

void ManagerClient::handleZoneServerList(const proto::mc::ZoneServerList &list) {
  std::vector<ZonePing::Server> servers;
  for (const auto &s : list.servers()) {
    ZonePing::Server zs;
    // Prefer a resolvable name for the TLS handshake, like the web client
    zs.host = !s.public_dns_name().empty() ? s.public_dns_name()
              : !s.dns_name().empty()      ? gameServerHostFromDnsName(s.dns_name())
                                           : s.host();
    zs.port = static_cast<int>(s.port());
    zs.serverId = s.server_id();
    servers.push_back(zs);
  }
  brls::Logger::info("Manager: pinging {} zone servers", servers.size());

  zonePing_.run(servers, [this](std::vector<ZonePing::Result> results) {
    if (finished_)
      return;
    if (onPingBestServer && !results.empty())
      onPingBestServer(results.front());
    proto::cm::ZoneServerPings pings;
    for (const auto &r : results) {
      auto *p = pings.add_server_pings();
      p->set_ping(r.pingMicros);
      p->set_server_id(r.serverId);
    }
    socket_.send(MessageType::CM_ZONE_SERVER_PINGS, pings);
  });
}

void ManagerClient::handleDirectionPlay(const proto::mc::DirectionPlay &dp) {
  GameServerAddress gs;
  gs.host = !dp.public_dns_name().empty() ? dp.public_dns_name()
            : !dp.dns_name().empty()      ? gameServerHostFromDnsName(dp.dns_name())
                                          : dp.ip();
  gs.port = dp.port();
  gs.ip = dp.ip();
  gs.userId = dp.user_id();
  gs.sessionId = dp.session_id();
  gs.connectId = dp.connect_id();
  gs.name = dp.name();
  gs.tokenSession = dp.token_session();
  gs.header = dp.header();
  gs.gameId = dp.game_id();
  gs.zoneId = dp.zone_id();
  gs.videoAdapterId = dp.video_adapter_id();
  gs.isDemo = dp.is_demo();
  gs.demoTimeSeconds = dp.demo_time_in_seconds();
  gs.isTimeBased = dp.is_time_based();
  gs.gameTimeSeconds = dp.game_time_in_seconds();
  gameServer_ = gs;
  brls::Logger::info(
    "Manager: MC_DIRECTION_PLAY -> {}:{} (session {}, connect {})",
    gs.host,
    gs.port,
    gs.sessionId,
    gs.connectId
  );
  if (onDirectionPlay)
    onDirectionPlay(gs);
}

void ManagerClient::reportServerConnection(bool success, const std::string &error) {
  if (!gameServer_) {
    brls::Logger::warning("Manager: reportServerConnection without MC_DIRECTION_PLAY");
    return;
  }
  proto::cm::ServerConnectionStatus status;
  status.set_ip_address(gameServer_->host);
  status.set_port(gameServer_->port);
  status.set_session_id(gameServer_->sessionId);
  status.set_connect_id(playUrl_.playTokenId);
  status.set_success(success);
  if (!success)
    status.set_error(error);
  socket_.send(MessageType::CM_CONNECT_SERVER, status);
}

void ManagerClient::handleFrame(const Frame &frame) {
  switch (frame.type) {
  case MessageType::M_ACCEPT:
    sendHandshake();
    break;

  case MessageType::M_CAPABILITIES: {
    proto::Capabilities caps;
    caps.ParseFromString(frame.payload);
    std::string list;
    for (const auto &c : caps.capabilities())
      list += (list.empty() ? "" : ",") + c;
    brls::Logger::info("Manager: server capabilities [{}]", list);
    break;
  }

  case MessageType::MC_VALIDATE_VERSION_RESPONSE: {
    proto::mc::ValidateVersionResponse r;
    r.ParseFromString(frame.payload);
    if (r.version() != CLIENT_SOFTWARE_VERSION)
      brls::Logger::warning(
        "Manager: server expects version {} (critical={}, url={})",
        r.version(),
        r.is_critical(),
        r.url_update()
      );
    if (r.has_language() && onLanguage)
      onLanguage(r.language() == proto::LANGUAGE_RU ? "ru" : "en");
    sendAuth();
    break;
  }

  case MessageType::MC_AUTH_SUCCESS:
    brls::Logger::info("Manager: authenticated");
    break;

  case MessageType::MC_AUTH_FAILED: {
    proto::mc::AuthFailed af;
    af.ParseFromString(frame.payload);
    SessionError e;
    e.kind = SessionError::Kind::BadAuthorization;
    e.code = af.reason();
    e.message = "Authorization failed (reason " + std::to_string(af.reason()) + ")";
    fail(e);
    break;
  }

  case MessageType::MC_ZONE_SERVER_LIST: {
    proto::mc::ZoneServerList list;
    list.ParseFromString(frame.payload);
    handleZoneServerList(list);
    break;
  }

  case MessageType::MC_IN_SERVER_QUEUE: {
    proto::mc::InServerQueue q;
    q.ParseFromString(frame.payload);
    // messagePart1..3 are "NN text" → slice(3)
    auto strip = [](const std::string &s) { return s.size() > 3 ? s.substr(3) : std::string(); };
    std::string message = strip(q.messagepart1());
    if (!q.messagepart2().empty())
      message += "\n" + strip(q.messagepart2());
    if (!q.messagepart3().empty())
      message += "\n" + strip(q.messagepart3());
    if (!q.queuenumbermessage().empty())
      message += "\n" + q.queuenumbermessage();
    if (onStatus)
      onStatus(q.header(), message, -1);
    break;
  }

  case MessageType::MC_USER_QUEUE_POSITION: {
    proto::mc::UserQueuePosition p;
    p.ParseFromString(frame.payload);
    if (onStatus)
      onStatus("", p.message(), -1);
    break;
  }

  case MessageType::MC_LOADING_PROFILES: {
    proto::mc::LoadingProfiles lp;
    lp.ParseFromString(frame.payload);
    if (onStatus)
      onStatus(lp.header(), lp.message(), lp.has_progress() ? lp.progress() : -1);
    break;
  }

  case MessageType::MC_DIRECTION_PLAY: {
    if (pingTestMode_) {
      brls::Logger::warning("Manager: ignoring MC_DIRECTION_PLAY in ping-test mode");
      break;
    }
    proto::mc::DirectionPlay dp;
    dp.ParseFromString(frame.payload);
    handleDirectionPlay(dp);
    break;
  }

  case MessageType::MC_CONNECT_SERVER_CONFIRMATION:
    brls::Logger::info("Manager: game server confirmed, sending M_BYE");
    close();
    finish(Result::Ok);
    break;

  case MessageType::MC_SUBSCRIPTION_PART_OF_DAY_EXPIRED: {
    proto::mc::SubscriptionPartOfDayEnd s;
    s.ParseFromString(frame.payload);
    SessionError e;
    e.kind = SessionError::Kind::SubscriptionExpired;
    e.title = s.header();
    e.message = s.text();
    fail(e);
    break;
  }

  case MessageType::MC_ALL_BUSY: {
    SessionError e;
    e.kind = SessionError::Kind::AllServersBusy;
    e.message = "All servers are busy";
    fail(e);
    break;
  }

  case MessageType::M_BYE: {
    proto::Bye bye;
    bye.ParseFromString(frame.payload);
    brls::Logger::info("Manager: M_BYE ({}, error={})", bye.reason(), bye.has_error());
    if (pingTestMode_) {
      finish(bye.has_error() ? Result::Failed : Result::Ok);
      socket_.close(1000, "BYE");
    }
    break;
  }

  case MessageType::M_ERROR: {
    proto::Error err;
    err.ParseFromString(frame.payload);
    fail(errorFromProto(err));
    break;
  }

  case MessageType::M_ERROR_INFO: {
    proto::ErrorInfo info;
    info.ParseFromString(frame.payload);
    brls::Logger::error(
      "Manager: M_ERROR_INFO component={} code={} {}",
      static_cast<int>(info.component_type()),
      info.error_code(),
      info.error_message()
    );
    break;
  }

  case MessageType::M_NOTIFICATION: {
    proto::Notification n;
    n.ParseFromString(frame.payload);
    if (onNotification)
      onNotification(n.message());
    break;
  }

  default:
    brls::Logger::debug("Manager: unhandled {}", messageTypeName(frame.type));
    break;
  }
}

} // namespace vkpcnx::stream
