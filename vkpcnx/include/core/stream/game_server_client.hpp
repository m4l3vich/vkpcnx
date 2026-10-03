#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "client_server.pb.h"
#include "core/stream/manager_client.hpp"
#include "core/stream/signalling_socket.hpp"
#include "protocol_common.pb.h"
#include "server_client.pb.h"

namespace vkpcnx::stream {

using StreamType = cg::network::protocol::StreamType;

// Game-server ("signalling") connection, protocol doc §5. Authenticates,
// exchanges capabilities, relays SDP/ICE for the WebRTC layer and turns the
// SC_* session messages into callbacks. Reconnects the socket on unclean
// drops (§5.4). All callbacks run on the UI thread.
class GameServerClient {
public:
  static constexpr int MAX_RECONNECT_ATTEMPTS = 3;
  // recover(): Wi-Fi takes a few seconds to come back after a Switch sleep
  static constexpr int RECOVERY_MAX_ATTEMPTS = 15;
  static constexpr int RECOVERY_RETRY_MS = 2000;

  enum class EndReason {
    ExitGame,
    DemoPlayEnd,
    GamePlayEnd,
    TrialTimeFinished,
    SubscriptionLimitExpired,
    SubscriptionExpired,
    SessionEvent, // SC_SESSION_EVENT type "session_end"
  };

  struct SessionEvent {
    std::string name, type, payload;
    int icon = 0;
  };

  GameServerClient();
  ~GameServerClient();

  // Socket opened and CS_AUTH sent → ManagerClient::reportServerConnection(true)
  std::function<void()> onConnected;
  // Socket could not be opened → ManagerClient::reportServerConnection(false, error)
  std::function<void(const std::string &error)> onConnectFailed;
  // SC_AUTH_SUCCESS + M_CAPABILITIES done: create peer connections and send offers
  // (§6.2). Not fired on re-handshakes after a reconnect (CS_RECONNECT is sent
  // instead), except after recover(), which needs fresh peer connections.
  std::function<void()> onReadyForOffers;
  std::function<void(StreamType type, const std::string &sdp, uint32_t controlToken)> onSdpAnswer;
  std::function<void(StreamType type, const std::string &iceJson)> onIceCandidate;
  std::function<void(EndReason reason)> onSessionEnd;
  std::function<void(const SessionError &error)> onError;
  std::function<void(const std::string &message)> onNotification;
  std::function<void(const std::string &text)> onStatus; // SC_GAME_UPDATE_BEGIN etc.
  std::function<void(const std::string &name)> onVmName; // SC_VM_NAME
  // §5.6: several SC_*_TIME_LEFT sources can fire together; reports whichever
  // will run out first, regardless of which source it came from.
  std::function<void(int64_t secondsLeft)> onTimeLeft;
  std::function<void()> onTimerDisabled;
  std::function<void()> onDisconnectStreams; // §5.5
  std::function<void()> onReconnectStreams;
  std::function<void(bool isShutdown)> onVmReboot;
  std::function<void(const SessionEvent &event)> onSessionEvent;
  std::function<void(const std::string &url)> onOpenUrl;
  // Reconnect progress (§5.4): attempt 2 shows a "reconnecting" modal
  std::function<void(int attempt)> onReconnecting;
  std::function<void()> onReconnected;
  // Socket closed for good (deliberately, or reconnects exhausted)
  std::function<void()> onClosed;

  void connect(const GameServerAddress &server);

  // §6.2 / §5.1
  void sendSdpOffer(StreamType type, const std::string &sdp, bool reconfigurate);
  void sendIceCandidate(StreamType type, const std::string &iceJson);
  // §5.2, flags: 3 = video+audio, 4 = inputs
  void sendStreamsStatus(uint32_t flags);

  // Mark the first SDP exchange as done: a later socket re-handshake sends
  // CS_RECONNECT and keeps the peer connections (§5.4).
  void setNegotiated(bool negotiated) { needReconnection_ = negotiated; }

  // §5.3 graceful close (M_CLIENT_BEFORE_CLOSE → confirmation → close)
  void gracefulClose(std::function<void()> done);
  void close();

  // The whole connection was lost (both peer connections failed, e.g. after
  // the Switch slept and every socket died): drops the signalling socket
  // without waiting on it, reconnects with RECOVERY_MAX_ATTEMPTS tries
  // RECOVERY_RETRY_MS apart, sends CS_RECONNECT after the re-handshake and
  // fires onReadyForOffers for fresh peer connections. Running out of tries
  // reports like any other reconnect: onError, then onClosed.
  void recover();
  // Drops the socket without a close handshake or callbacks, so connect()
  // can start over with another address (re-join through the manager)
  void abandon();

  bool isOpen() const { return socket_.isOpen(); }
  const GameServerAddress &server() const { return server_; }
  int logLevel() const { return logLevel_; }

private:
  void handleFrame(const Frame &frame);
  void sendAuth();
  void scheduleReconnect();
  void emitTimeLeft();

  SignallingSocket socket_;
  GameServerAddress server_;
  std::optional<int64_t> demoTimeLeft_, gameTimeLeft_, subscriptionLimitLeft_,
    subscriptionDailyLeft_;
  bool everConnected_ = false;
  bool authenticated_ = false;
  bool needReconnection_ = false;
  bool recovering_ = false; // recover() in progress: fresh offers after the re-handshake
  bool closing_ = false;
  int reconnectAttempt_ = 0;
  int logLevel_ = 0;
  size_t reconnectDelay_ = 0;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace vkpcnx::stream
