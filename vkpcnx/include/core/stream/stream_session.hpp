#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "core/media/audio_decoder.hpp"
#include "core/media/audio_output.hpp"
#include "core/media/video_decoder.hpp"
#include "core/stream/game_server_client.hpp"
#include "core/stream/input_channel.hpp"
#include "core/stream/manager_client.hpp"
#include "core/stream/play_url.hpp"
#include "core/stream/webrtc_session.hpp"
#include "core/utils/periodic_timer.hpp"

namespace vkpcnx::stream {

// Ties everything together for one play session (protocol doc §12):
// manager → game server → two peer connections → input channel, plus the
// decoders. Lives on the UI thread; callbacks from network threads are
// marshalled with brls::sync. All public callbacks run on the UI thread.
class StreamSession {
public:
  enum class State {
    Idle,
    ConnectingManager,
    WaitingForServer, // queue / profile loading
    ConnectingGameServer,
    Negotiating, // offers sent, waiting for the peer connections
    Streaming,
    Reconnecting, // signalling socket dropped (§5.4), or the whole connection is being recovered
    Ended,
    Failed,
  };

  struct Config {
    PlayUrl playUrl;
    ManagerClient::Options manager;
    WebRtcSession::VideoConfig video;
  };

  static constexpr int CONNECT_TIMEOUT_MS = 120000;
  static constexpr int RECOVERY_CONNECT_TIMEOUT_MS = 30000; // offers made by a recovery
  // Recovery step 1 waits this long for each answer before step 2
  static constexpr int RECOVERY_ANSWER_TIMEOUT_MS = 10000;
  static constexpr int VM_REBOOT_CONNECT_TIMEOUT_MS = 180000;
  static constexpr int NO_VIDEO_TIMEOUT_MS = 180000;
  // §6.4 freeze detection: seconds with no decoded frames while data still arrives
  static constexpr int FREEZE_SAMPLES = 10;

  StreamSession();
  ~StreamSession();

  std::function<void(State state)> onStateChange;
  std::function<void(const std::string &header, const std::string &text, int progress)> onStatus;
  std::function<void(const SessionError &error)> onError;
  std::function<void(const std::string &message)> onNotification;
  std::function<void(int64_t secondsLeft)> onTimeLeft;
  std::function<void()> onTimerDisabled;
  std::function<void(GameServerClient::EndReason reason)> onSessionEnd;
  // Ping-test mode: forwarded from ManagerClient::onPingProgress
  std::function<void(int completed, int total)> onPingProgress;
  // Ping-test mode: forwarded from ManagerClient::onPingBestServer
  std::function<void(const ZonePing::Result &best)> onPingBestServer;
  std::function<void(const CursorImage &cursor)> onCursor;
  std::function<void(const std::string &text)> onClipboardText;
  std::function<void(const std::string &url)> onOpenUrl;
  std::function<void(const GameServerClient::SessionEvent &event)> onSessionEvent;
  std::function<void(const std::string &name)> onVmName;

  void start(const Config &config);
  // §5.3 destroyApp: tear down media, before-close handshake, close sockets
  void stop(std::function<void()> done = nullptr);
  // §6.4: renegotiate the video peer connection with new settings
  void reconfigureVideo(const WebRtcSession::VideoConfig &video);

  // The renderer reports the first drawn frame (→ CS_STREAMS_STATUS) and the
  // rendered rectangle (→ CS_MOUSE_SETTINGS)
  void notifyVideoStarted();
  void setVideoRect(int width, int height);

  State state() const { return state_; }
  bool isStreaming() const { return state_ == State::Streaming; }
  InputChannel &input() { return input_; }
  media::VideoDecoder &decoder() { return decoder_; }
  const WebRtcSession &rtc() const { return rtc_; }
  const Config &config() const { return config_; }
  int64_t sessionId() const { return gameServer_.server().sessionId; }
  const GameServerAddress &gameServerAddress() const { return gameServer_.server(); }

  struct Stats {
    double inputRttMs = 0;
    std::optional<std::chrono::milliseconds> rtt;
    size_t videoBytes = 0;
    uint64_t rtpPacketsReceived = 0;
    uint64_t rtpPacketsLost = 0;
    uint64_t keyframeRequests = 0;
    media::VideoDecoder::Stats decoder;
    size_t audioBufferedFrames = 0;
  };
  Stats stats() const;

private:
  void setState(State state);
  void fail(const SessionError &error);
  void createPeerConnections(bool reconfigurate, int connectTimeoutMs);
  // A peer connection failed mid-session (after a Switch sleep, every socket
  // has): rebuild the signalling socket and both peer connections. The real
  // server ignores plain fresh offers on a reconnected socket, so this is a
  // ladder: step 1 sends the video offer as a §6.4 reconfiguration on the
  // reconnected socket and the inputs offer once that video is up; an answer
  // missing → step 2 (rejoinViaManager) repeats the launch handshake through
  // the manager with the same play_url.
  void recoverConnection(StreamType lost);
  void rejoinViaManager();
  void armRecoveryTimer();
  void startConnectTimer(int ms);
  void cancelTimers();
  void checkStreamsStatus();
  void teardownMedia();
  template <class F> void sync(F fn);

  Config config_;
  State state_ = State::Idle;
  ManagerClient manager_;
  GameServerClient gameServer_;
  WebRtcSession rtc_;
  InputChannel input_;
  media::VideoDecoder decoder_;
  media::AudioDecoder audioDecoder_;
  media::AudioOutput audioOutput_;

  bool videoOfferSent_ = false;
  bool inputsOfferPending_ = false;
  bool videoAnswered_ = false;
  bool inputsAnswered_ = false;
  bool videoStarted_ = false;
  bool streamsStatusSent_ = false;
  bool reconfiguring_ = false;
  // recoverConnection() ran and the new peer connections don't exist yet:
  // further failures are the old ones going down
  bool recovering_ = false;
  enum class Recovery { None, Reconnecting, ReconfigureOffers, Rejoining };
  Recovery recovery_ = Recovery::None;
  size_t recoveryTimer_ = 0;
  bool inputsAfterVideo_ = false; // recovery step 1: inputs offer waits for the video PC
  uint32_t inputToken_ = 0;
  int videoRectW_ = 0, videoRectH_ = 0;
  size_t connectTimer_ = 0;
  size_t noVideoTimer_ = 0;
  bool stopping_ = false;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  std::vector<int16_t> pcm_;

  // 1 s statistics sampler (freeze detection, §6.4)
  void sampleStats();
  vkpcnx::utils::PeriodicTimer statsTimer_;
  uint64_t lastFrames_ = 0;
  size_t lastBytes_ = 0;
  int frozenSamples_ = 0;
};

const char *streamStateName(StreamSession::State state);

} // namespace vkpcnx::stream
