#include "core/stream/stream_session.hpp"

#include <borealis.hpp>

namespace vkpcnx::stream {

namespace proto = cg::network::protocol;

const char *streamStateName(StreamSession::State state) {
  switch (state) {
  case StreamSession::State::Idle:
    return "Idle";
  case StreamSession::State::ConnectingManager:
    return "ConnectingManager";
  case StreamSession::State::WaitingForServer:
    return "WaitingForServer";
  case StreamSession::State::ConnectingGameServer:
    return "ConnectingGameServer";
  case StreamSession::State::Negotiating:
    return "Negotiating";
  case StreamSession::State::Streaming:
    return "Streaming";
  case StreamSession::State::Reconnecting:
    return "Reconnecting";
  case StreamSession::State::Ended:
    return "Ended";
  case StreamSession::State::Failed:
    return "Failed";
  }
  return "?";
}

// Runs fn on the UI thread unless the session is gone by then
template <class F> void StreamSession::sync(F fn) {
  std::weak_ptr<bool> alive = alive_;
  brls::sync([alive, fn = std::move(fn)] {
    if (auto a = alive.lock(); a && *a)
      fn();
  });
}

StreamSession::StreamSession() : input_(rtc_) {
  // ---- manager ----
  manager_.onStatus = [this](const std::string &h, const std::string &m, int p) {
    if (state_ == State::ConnectingManager)
      setState(State::WaitingForServer);
    if (onStatus)
      onStatus(h, m, p);
  };
  manager_.onNotification = [this](const std::string &m) {
    if (onNotification)
      onNotification(m);
  };
  manager_.onError = [this](const SessionError &e) { fail(e); };
  manager_.onDirectionPlay = [this](const GameServerAddress &server) {
    gameServer_.connect(server);
    setState(State::ConnectingGameServer);
  };
  manager_.onPingProgress = [this](int completed, int total) {
    if (onPingProgress)
      onPingProgress(completed, total);
  };
  manager_.onPingBestServer = [this](const ZonePing::Result &best) {
    if (onPingBestServer)
      onPingBestServer(best);
  };
  manager_.onFinished = [this](ManagerClient::Result result) {
    if (config_.playUrl.workMode == PlayUrl::WorkMode::PingTest) {
      setState(result == ManagerClient::Result::Ok ? State::Ended : State::Failed);
    }
  };

  // ---- game server ----
  gameServer_.onConnected = [this] { manager_.reportServerConnection(true); };
  gameServer_.onConnectFailed = [this](const std::string &error) {
    manager_.reportServerConnection(false, error);
    SessionError e;
    e.kind = SessionError::Kind::ConnectionFailed;
    e.message = "Game server connection failed: " + error;
    fail(e);
  };
  gameServer_.onReadyForOffers = [this] {
    setState(State::Negotiating);
    createPeerConnections(false, CONNECT_TIMEOUT_MS);
  };
  gameServer_.onSdpAnswer = [this](StreamType type, const std::string &sdp, uint32_t token) {
    rtc_.setRemoteAnswer(type, sdp);
    if (type == proto::ST_INPUTS) {
      inputToken_ = token;
      inputsAnswered_ = true;
    } else {
      videoAnswered_ = true;
    }
    if (videoAnswered_ && inputsAnswered_)
      gameServer_.setNegotiated(true); // §5.4 / §6.2
  };
  gameServer_.onIceCandidate = [this](StreamType type, const std::string &ice) {
    rtc_.addRemoteCandidate(type, ice);
  };
  gameServer_.onSessionEnd = [this](GameServerClient::EndReason reason) {
    brls::Logger::info("StreamSession: session ended ({})", static_cast<int>(reason));
    if (onSessionEnd)
      onSessionEnd(reason);
    stop();
  };
  gameServer_.onError = [this](const SessionError &e) {
    if (onError)
      onError(e);
  };
  gameServer_.onNotification = [this](const std::string &m) {
    if (onNotification)
      onNotification(m);
  };
  gameServer_.onStatus = [this](const std::string &text) {
    if (state_ != State::Streaming && onStatus)
      onStatus("", text, -1);
  };
  gameServer_.onTimeLeft = [this](int64_t s) {
    if (onTimeLeft)
      onTimeLeft(s);
  };
  gameServer_.onTimerDisabled = [this] {
    if (onTimerDisabled)
      onTimerDisabled();
  };
  gameServer_.onDisconnectStreams = [this] {
    input_.stop();
    rtc_.closeAll();
    cancelTimers();
  };
  gameServer_.onReconnectStreams = [this] {
    setState(State::Negotiating);
    createPeerConnections(false, CONNECT_TIMEOUT_MS);
  };
  gameServer_.onVmReboot = [this](bool) {
    input_.stop();
    rtc_.closeAll();
    streamsStatusSent_ = false;
    videoStarted_ = false;
    setState(State::Negotiating);
    if (onStatus)
      onStatus("", "Перезагрузка виртуальной машины…", -1);
    createPeerConnections(false, VM_REBOOT_CONNECT_TIMEOUT_MS);
  };
  gameServer_.onSessionEvent = [this](const GameServerClient::SessionEvent &ev) {
    if (onSessionEvent)
      onSessionEvent(ev);
  };
  gameServer_.onOpenUrl = [this](const std::string &url) {
    if (onOpenUrl)
      onOpenUrl(url);
  };
  gameServer_.onVmName = [this](const std::string &name) {
    if (onVmName)
      onVmName(name);
  };
  gameServer_.onReconnecting = [this](int attempt) {
    if (attempt < 2)
      return;
    setState(State::Reconnecting);
    if (onStatus)
      onStatus(
        "",
        "Сигнальный WebSocket игрового сервера разорван, переподключение (попытка " +
          std::to_string(attempt) + ")…",
        -1
      );
  };
  gameServer_.onReconnected = [this] {
    if (state_ == State::Reconnecting)
      setState(streamsStatusSent_ ? State::Streaming : State::Negotiating);
  };
  gameServer_.onClosed = [this] {
    if (!stopping_ && state_ != State::Ended && state_ != State::Failed) {
      SessionError e;
      e.kind = SessionError::Kind::RemoteHostNotResponding;
      e.message = "Signalling connection lost";
      fail(e);
    }
  };

  // ---- WebRTC (callbacks on libdatachannel threads) ----
  rtc_.onOffer = [this](StreamType type, const std::string &sdp) {
    sync([this, type, sdp] {
      gameServer_.sendSdpOffer(type, sdp, type == proto::ST_VIDEO && reconfiguring_);
      if (type == proto::ST_VIDEO) {
        videoOfferSent_ = true;
        reconfiguring_ = false;
        if (inputsOfferPending_) {
          // §6.2: video first (awaited), then inputs
          inputsOfferPending_ = false;
          rtc_.createInputs();
        }
      }
    });
  };
  rtc_.onLocalCandidate = [this](StreamType type, const std::string &ice) {
    sync([this, type, ice] { gameServer_.sendIceCandidate(type, ice); });
  };
  rtc_.onStateChange = [this](StreamType type, rtc::PeerConnection::State s) {
    sync([this, type, s] {
      if (s == rtc::PeerConnection::State::Connected) {
        checkStreamsStatus();
      } else if (s == rtc::PeerConnection::State::Failed) {
        SessionError e;
        e.kind = SessionError::Kind::RemoteHostNotResponding;
        e.message = std::string(type == proto::ST_VIDEO ? "Video" : "Input") + " connection failed";
        fail(e);
      }
    });
  };
  rtc_.onVideoFrame = [this](const rtc::binary &frame, uint32_t ts) {
    decoder_.push(reinterpret_cast<const uint8_t *>(frame.data()), frame.size(), ts);
  };
  rtc_.onAudioFrame = [this](const rtc::binary &packet, uint32_t) {
    if (!audioDecoder_.isOpen())
      return;
    size_t frames =
      audioDecoder_.decode(reinterpret_cast<const uint8_t *>(packet.data()), packet.size(), pcm_);
    if (frames && audioOutput_.isOpen())
      audioOutput_.write(pcm_.data(), frames);
  };
  rtc_.onDataChannelOpen = [this] {
    sync([this] {
      if (inputToken_ == 0)
        brls::Logger::warning("StreamSession: data channel open before the input token arrived");
      input_.start(inputToken_);
    });
  };
  rtc_.onDataChannelMessage = [this](const rtc::binary &msg) { input_.onMessage(msg); };
  rtc_.onDataChannelClosed = [this](const std::string &reason) {
    sync([this, reason] {
      brls::Logger::warning("StreamSession: input channel closed ({})", reason);
      input_.stop();
    });
  };

  // ---- input channel (callbacks on its / libdatachannel threads) ----
  input_.onTokenAccepted = [this] {
    sync([this] {
      if (videoRectW_ > 0)
        input_.setWindowSize(videoRectW_, videoRectH_);
    });
  };
  input_.onCursor = [this](const CursorImage &c) {
    sync([this, c] {
      if (onCursor)
        onCursor(c);
    });
  };
  input_.onClipboardText = [this](const std::string &text) {
    sync([this, text] {
      if (onClipboardText)
        onClipboardText(text);
    });
  };
  input_.onError = [this](const std::string &m) {
    sync([this, m] {
      SessionError e;
      e.message = m;
      if (onError)
        onError(e);
    });
  };

  decoder_.onDecodeError = [this] { rtc_.requestKeyframe(); };
  // The hardware decoder doesn't always flag frames with missing slices, and
  // the damage then persists in every later frame of a static picture
  rtc_.onVideoLoss = [this] { decoder_.resync(); };
}

StreamSession::~StreamSession() {
  *alive_ = false;
  statsTimer_.stop();
  cancelTimers();
  input_.stop();
  rtc_.closeAll();
  teardownMedia();
}

void StreamSession::setState(State state) {
  if (state_ == state)
    return;
  brls::Logger::info("StreamSession: {} -> {}", streamStateName(state_), streamStateName(state));
  state_ = state;
  if (onStateChange)
    onStateChange(state);
}

void StreamSession::start(const Config &config) {
  config_ = config;
  stopping_ = false;
  videoOfferSent_ = inputsOfferPending_ = videoAnswered_ = inputsAnswered_ = false;
  videoStarted_ = streamsStatusSent_ = reconfiguring_ = false;
  inputToken_ = 0;

  if (config.playUrl.workMode != PlayUrl::WorkMode::PingTest) {
    if (!decoder_.open())
      brls::Logger::error("StreamSession: video decoder unavailable");
    if (!audioDecoder_.open() || !audioOutput_.open())
      brls::Logger::error("StreamSession: audio unavailable");
  }
  setState(State::ConnectingManager);
  manager_.connect(config.playUrl, config.manager);

  if (config.playUrl.workMode != PlayUrl::WorkMode::PingTest) {
    lastFrames_ = 0;
    lastBytes_ = 0;
    frozenSamples_ = 0;
    statsTimer_.start(std::chrono::seconds(1), [this] { sync([this] { sampleStats(); }); });
  }
}

void StreamSession::sampleStats() {
  if (state_ != State::Streaming)
    return;
  uint64_t frames = decoder_.stats().framesDecoded;
  size_t bytes = rtc_.stats().videoBytesReceived;
  bool noFrames = frames == lastFrames_;
  bool dataFlowing = bytes > lastBytes_ + 1250; // > 0.01 Mbit in the last second
  lastFrames_ = frames;
  lastBytes_ = bytes;
  frozenSamples_ = (noFrames && dataFlowing) ? frozenSamples_ + 1 : 0;
  if (frozenSamples_ >= FREEZE_SAMPLES) {
    brls::Logger::warning("StreamSession: video frozen for {} s, renegotiating", frozenSamples_);
    frozenSamples_ = 0;
    reconfigureVideo(config_.video);
  }
}

void StreamSession::createPeerConnections(bool reconfigurate, int connectTimeoutMs) {
  videoAnswered_ = inputsAnswered_ = false;
  videoOfferSent_ = false;
  reconfiguring_ = reconfigurate;
  inputsOfferPending_ = true;
  startConnectTimer(connectTimeoutMs);
  rtc_.createVideo(config_.video);
}

void StreamSession::reconfigureVideo(const WebRtcSession::VideoConfig &video) {
  // §6.4: only the video PC is recreated; inputs are untouched
  config_.video = video;
  videoAnswered_ = false;
  reconfiguring_ = true;
  inputsOfferPending_ = false;
  videoStarted_ = false;
  startConnectTimer(CONNECT_TIMEOUT_MS);
  rtc_.createVideo(config_.video);
}

void StreamSession::startConnectTimer(int ms) {
  cancelTimers();
  std::weak_ptr<bool> alive = alive_;
  connectTimer_ = brls::delay(ms, [this, alive] {
    if (auto a = alive.lock(); !a || !*a)
      return;
    connectTimer_ = 0;
    if (!rtc_.isVideoConnected() || !rtc_.isInputsConnected()) {
      SessionError e;
      e.kind = SessionError::Kind::RemoteHostNotResponding;
      e.message = "Remote host is not responding";
      fail(e);
    }
  });
}

void StreamSession::cancelTimers() {
  if (connectTimer_) {
    brls::cancelDelay(connectTimer_);
    connectTimer_ = 0;
  }
  if (noVideoTimer_) {
    brls::cancelDelay(noVideoTimer_);
    noVideoTimer_ = 0;
  }
}

void StreamSession::checkStreamsStatus() {
  if (streamsStatusSent_)
    return;
  bool video = rtc_.isVideoConnected();
  bool inputs = rtc_.isInputsConnected();
  if (!video || !inputs)
    return;
  if (connectTimer_) {
    brls::cancelDelay(connectTimer_);
    connectTimer_ = 0;
  }
  if (videoStarted_) {
    // §5.2
    gameServer_.sendStreamsStatus((video ? 3 : 0) | (inputs ? 4 : 0));
    streamsStatusSent_ = true;
    setState(State::Streaming);
    return;
  }
  if (!noVideoTimer_) {
    if (onStatus)
      onStatus("", "WebRTC-соединения установлены, ожидание первого видеокадра от сервера…", -1);
    std::weak_ptr<bool> alive = alive_;
    noVideoTimer_ = brls::delay(NO_VIDEO_TIMEOUT_MS, [this, alive] {
      if (auto a = alive.lock(); !a || !*a)
        return;
      noVideoTimer_ = 0;
      if (!streamsStatusSent_) {
        gameServer_.sendStreamsStatus(7);
        streamsStatusSent_ = true;
        SessionError e;
        e.kind = SessionError::Kind::LowBandwidth;
        e.message = "No video received (UDP may be blocked)";
        if (onError)
          onError(e);
      }
    });
  }
}

void StreamSession::notifyVideoStarted() {
  videoStarted_ = true;
  if (noVideoTimer_) {
    brls::cancelDelay(noVideoTimer_);
    noVideoTimer_ = 0;
  }
  if (state_ != State::Streaming && streamsStatusSent_)
    setState(State::Streaming);
  checkStreamsStatus();
}

void StreamSession::setVideoRect(int width, int height) {
  videoRectW_ = width;
  videoRectH_ = height;
  input_.setWindowSize(width, height);
}

void StreamSession::fail(const SessionError &error) {
  if (state_ == State::Failed || state_ == State::Ended)
    return;
  brls::Logger::error("StreamSession: failed: {}", error.message);
  setState(State::Failed);
  if (onError)
    onError(error);
  stop();
}

void StreamSession::teardownMedia() {
  decoder_.close();
  audioOutput_.close();
  audioDecoder_.close();
}

void StreamSession::stop(std::function<void()> done) {
  if (stopping_) {
    if (done)
      done();
    return;
  }
  stopping_ = true;
  cancelTimers();
  statsTimer_.stop();
  if (state_ != State::Failed)
    setState(State::Ended);

  // 1. peer connections and media
  input_.stop();
  rtc_.closeAll();
  teardownMedia();

  // 2./3. before-close handshake on every open socket, then close
  auto remaining = std::make_shared<int>(0);
  auto finish = [remaining, done] {
    if (--*remaining == 0 && done)
      done();
  };
  if (gameServer_.isOpen()) {
    (*remaining)++;
    gameServer_.gracefulClose(finish);
  }
  if (manager_.isOpen()) {
    (*remaining)++;
    manager_.gracefulClose(finish);
  }
  if (*remaining == 0 && done)
    done();
}

StreamSession::Stats StreamSession::stats() const {
  Stats s;
  s.inputRttMs = input_.lastInputRttMs();
  auto r = rtc_.stats();
  s.rtt = r.videoRtt;
  s.videoBytes = r.videoBytesReceived;
  s.rtpPacketsReceived = r.rtpPacketsReceived;
  s.rtpPacketsLost = r.rtpPacketsLost;
  s.keyframeRequests = r.keyframeRequests;
  s.decoder = decoder_.stats();
  s.audioBufferedFrames = audioOutput_.bufferedFrames();
  return s;
}

} // namespace vkpcnx::stream
