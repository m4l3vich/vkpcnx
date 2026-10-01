#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "client_manager.pb.h"
#include "core/stream/play_url.hpp"
#include "core/stream/signalling_socket.hpp"
#include "core/stream/zone_ping.hpp"
#include "manager_client.pb.h"
#include "protocol_common.pb.h"

namespace vkpcnx::stream {

// Identity the web player reports to the manager (protocol doc §4)
constexpr const char *CLIENT_SOFTWARE_NAME = "webrtc-mailru";
constexpr int64_t CLIENT_SOFTWARE_VERSION = 48103567;

// Client-side view of a session error, mapped from M_ERROR / MC_AUTH_FAILED
// and the various SC_* end-of-session messages.
struct SessionError {
  enum class Kind {
    Generic,
    BadAuthorization,
    AllServersBusy,
    DuplicateClient,
    SessionTimeout,
    LowBandwidth,
    TooFrequentConnection,
    NeedUpdate,
    RemoteHostNotResponding,
    SubscriptionExpired,
    ConnectionFailed,
  };
  Kind kind = Kind::Generic;
  std::string title;
  std::string message;
  int code = 0; // ErrorType value when known
};

SessionError errorFromProto(const cg::network::protocol::Error &err);

// Where the game server lives, as told by MC_DIRECTION_PLAY (§4, §5)
struct GameServerAddress {
  std::string host; // resolved per the public_dns_name / dns_name / ip rule
  int port = 0;
  std::string ip; // DirectionPlay.ip, echoed in CS_SDP_OFFER.server_ip
  int64_t userId = 0;
  int64_t sessionId = 0;
  int64_t connectId = 0;
  std::string name;         // bytes
  std::string tokenSession; // bytes
  std::string header;
  int64_t gameId = 0;
  int64_t zoneId = 0;
  int64_t videoAdapterId = 0;
  bool isDemo = false;
  int64_t demoTimeSeconds = 0;
  bool isTimeBased = false;
  int64_t gameTimeSeconds = 0;

  std::string url() const { return "wss://" + host + ":" + std::to_string(port); }
};

// Manager ("wss://<host>:<port>" from play_url) state machine, protocol doc §4.
// Drives the handshake up to MC_DIRECTION_PLAY; the owner then opens the
// game-server socket and calls reportServerConnection(). Also implements the
// ping-test work mode (§4.2). All callbacks run on the UI thread.
class ManagerClient {
public:
  struct Options {
    int monitorWidth = 1920; // physical pixels of the display
    int monitorHeight = 1080;
    bool ios = false;       // report WebRtcIOS instead of WebRtc
    int bitrateMinMbit = 0; // 0 = not sent
    int bitrateMaxMbit = 0;
    int numberOfSlices = 0;
    int numberOfRefFrames = 0;
  };

  enum class Result { Ok, Failed };

  ManagerClient();
  ~ManagerClient();

  // Loading-screen text (queue position, profile loading …)
  std::function<void(const std::string &header, const std::string &message, int progress)> onStatus;
  std::function<void(const std::string &message)> onNotification;
  std::function<void(const SessionError &error)> onError;
  // Game-stream mode: connect to the game server now (§5), then call
  // reportServerConnection()
  std::function<void(const GameServerAddress &server)> onDirectionPlay;
  // Game-stream mode: MC_CONNECT_SERVER_CONFIRMATION received, socket closed.
  // Ping-test mode: M_BYE received (Ok) or any failure (Failed).
  std::function<void(Result result)> onFinished;
  // Language switch requested by MC_VALIDATE_VERSION_RESPONSE ("ru"/"en")
  std::function<void(const std::string &language)> onLanguage;
  // Ping-test mode (§4.1): fired as each zone server answers, then once more
  // when the last one does
  std::function<void(int completed, int total)> onPingProgress;
  // Ping-test mode: the winning (lowest-ping) server, once all have answered
  std::function<void(const ZonePing::Result &best)> onPingBestServer;

  void connect(const PlayUrl &playUrl, const Options &options);

  // §4: CM_CONNECT_SERVER after the game-server socket opened (or failed)
  void reportServerConnection(bool success, const std::string &error = {});

  // Sends M_BYE and closes (used when the session is aborted by the user)
  void close();
  // §5.3 graceful close (M_CLIENT_BEFORE_CLOSE handshake); `done` on the UI thread
  void gracefulClose(std::function<void()> done);

  bool isOpen() const { return socket_.isOpen(); }

private:
  void handleFrame(const Frame &frame);
  void sendHandshake();
  void sendAuth();
  void handleZoneServerList(const cg::network::protocol::mc::ZoneServerList &list);
  void handleDirectionPlay(const cg::network::protocol::mc::DirectionPlay &dp);
  void fail(SessionError error);
  void finish(Result result);

  SignallingSocket socket_;
  ZonePing zonePing_;
  PlayUrl playUrl_;
  Options options_;
  bool pingTestMode_ = false;
  bool finished_ = false;
  bool byeSent_ = false;
  std::optional<GameServerAddress> gameServer_;
  size_t playWatchdog_ = 0;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

// "a.b.c" → "b.clgrtc.ru" (§4, NA(dns))
std::string gameServerHostFromDnsName(const std::string &dnsName);
// Snap a display size to the nearest standard WxH within ±5 px (§4)
std::pair<int, int> snapMonitorSize(int width, int height);

} // namespace vkpcnx::stream
