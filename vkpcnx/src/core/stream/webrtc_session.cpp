#include "core/stream/webrtc_session.hpp"

#include <borealis/core/logger.hpp>
#include <chrono>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <sstream>

namespace vkpcnx::stream {

namespace proto = cg::network::protocol;

// Chrome's H.264 payload-type layout (§6.2 (a)); RTX = primary + 1
struct H264Offer {
  int pt;
  int rtxPt;
  const char *profileLevelId;
};
static const H264Offer H264_OFFERS[] = {
  {102, 103, "64001f"}, // High
  {104, 105, "640c1f"}, // ConstrainedHigh
  {106, 107, "4d001f"}, // Main
  {108, 109, "42001f"}, // Base
  {127, 125, "42e01f"}, // ConstrainedBase
};

// Logs video RTP sequence gaps once a second. Chained last so it sees the raw
// packets before the depacketizer; tells network loss apart from decoder errors.
class RtpLossCounter final : public rtc::MediaHandler {
public:
  // onPackets(received, lost) runs once per incoming batch, for the stats
  RtpLossCounter(std::function<void()> onLoss, std::function<void(uint64_t, uint64_t)> onPackets)
      : onLoss_(std::move(onLoss)), onPackets_(std::move(onPackets)) {}

  void incoming(rtc::message_vector &messages, const rtc::message_callback &) override {
    uint64_t batchReceived = 0, batchLost = 0;
    for (const auto &m : messages) {
      if (m->type != rtc::Message::Binary || m->size() < sizeof(rtc::RtpHeader))
        continue;
      auto *h = reinterpret_cast<const rtc::RtpHeader *>(m->data());
      bool video = false;
      for (const auto &o : H264_OFFERS)
        video |= h->payloadType() == o.pt;
      if (!video)
        continue;
      uint16_t seq = h->seqNumber();
      if (started_) {
        auto gap = static_cast<uint16_t>(seq - static_cast<uint16_t>(last_ + 1));
        if (gap == 0 || gap < 0x8000) { // ahead: in order or after a gap
          lost_ += gap;
          batchLost += gap;
          last_ = seq;
          if (gap && onLoss_)
            onLoss_();
        } // behind: reordered or duplicate, keep last_
      } else {
        started_ = true;
        last_ = seq;
      }
      received_++;
      batchReceived++;
    }
    if (onPackets_ && (batchReceived || batchLost))
      onPackets_(batchReceived, batchLost);
    auto now = std::chrono::steady_clock::now();
    if (now - since_ >= std::chrono::seconds(1)) {
      if (lost_ > 0)
        brls::Logger::warning(
          "Video RTP: {} of {} packets lost in the last second", lost_, lost_ + received_
        );
      lost_ = received_ = 0;
      since_ = now;
    }
  }

private:
  std::function<void()> onLoss_;
  std::function<void(uint64_t, uint64_t)> onPackets_;
  bool started_ = false;
  uint16_t last_ = 0;
  uint64_t lost_ = 0, received_ = 0;
  std::chrono::steady_clock::time_point since_ = std::chrono::steady_clock::now();
};
static constexpr int OPUS_PT = 111;

void WebRtcSession::initLogging() {
  static bool done = false;
  if (done)
    return;
  done = true;
  rtc::LogLevel level = rtc::LogLevel::Info;
  if (const char *env = std::getenv("VKPCNX_RTC_LOG")) {
    std::string v = env;
    level = v == "verbose" ? rtc::LogLevel::Verbose : v == "debug" ? rtc::LogLevel::Debug : level;
  }
  rtc::InitLogger(level, [](rtc::LogLevel level, std::string message) {
    switch (level) {
    case rtc::LogLevel::Fatal:
    case rtc::LogLevel::Error:
      brls::Logger::error("rtc: {}", message);
      break;
    case rtc::LogLevel::Warning:
      brls::Logger::warning("rtc: {}", message);
      break;
    case rtc::LogLevel::Info:
      brls::Logger::info("rtc: {}", message);
      break;
    default:
      brls::Logger::debug("rtc: {}", message);
      break;
    }
  });
}

WebRtcSession::WebRtcSession() { initLogging(); }

WebRtcSession::~WebRtcSession() { closeAll(); }

std::string WebRtcSession::candidateJson(const rtc::Candidate &c, const std::string &ufrag) {
  nlohmann::json j;
  j["candidate"] = c.candidate();
  j["sdpMid"] = c.mid();
  j["sdpMLineIndex"] = std::atoi(c.mid().c_str()); // mids are "0"/"1", matching their index
  j["usernameFragment"] = ufrag;
  return j.dump();
}

std::shared_ptr<rtc::PeerConnection>
WebRtcSession::makePeerConnection(StreamType type, uint32_t generation) {
  rtc::Configuration config;
  config.iceServers.emplace_back(STUN_SERVER);
  config.disableAutoNegotiation = true;
  config.enableIceTcp = false; // §6.1: only UDP candidates are used
  auto pc = std::make_shared<rtc::PeerConnection>(config);
  const char *name = type == proto::ST_VIDEO ? "VideoAndAudioStream" : "InputStream";

  pc->onLocalCandidate([this,
                        type,
                        generation,
                        name,
                        weak = std::weak_ptr<rtc::PeerConnection>(pc)](rtc::Candidate candidate) {
    if (candidate.transportType() != rtc::Candidate::TransportType::Udp &&
        candidate.transportType() != rtc::Candidate::TransportType::Unknown)
      return;
    std::string ufrag;
    if (auto pc = weak.lock())
      if (auto local = pc->localDescription())
        ufrag = local->iceUfrag().value_or("");
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const Peer &peer = type == proto::ST_VIDEO ? video_ : inputs_;
      if (peer.generation != generation)
        return;
    }
    brls::Logger::debug("{}: local candidate {}", name, candidate.candidate());
    if (onLocalCandidate)
      onLocalCandidate(type, candidateJson(candidate, ufrag));
  });

  pc->onStateChange([this, type, generation, name](rtc::PeerConnection::State state) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const Peer &peer = type == proto::ST_VIDEO ? video_ : inputs_;
      if (peer.generation != generation)
        return;
    }
    std::ostringstream oss;
    oss << state;
    brls::Logger::info("{}: state {}", name, oss.str());
    bool connected = state == rtc::PeerConnection::State::Connected;
    if (type == proto::ST_VIDEO)
      videoConnected_ = connected;
    else
      inputsConnected_ = connected;
    if (onStateChange)
      onStateChange(type, state);
  });

  pc->onGatheringStateChange([name](rtc::PeerConnection::GatheringState state) {
    std::ostringstream oss;
    oss << state;
    brls::Logger::debug("{}: gathering {}", name, oss.str());
  });

  pc->onIceStateChange([name, weak = std::weak_ptr<rtc::PeerConnection>(pc)](
                         rtc::PeerConnection::IceState state
                       ) {
    std::ostringstream oss;
    oss << state;
    brls::Logger::info("{}: ICE {}", name, oss.str());
    // Which path ICE settled on (host / srflx / relay) explains most
    // "connects but no video" reports
    auto pc = weak.lock();
    rtc::Candidate local, remote;
    if (pc && state == rtc::PeerConnection::IceState::Connected &&
        pc->getSelectedCandidatePair(&local, &remote))
      brls::Logger::info(
        "{}: selected pair {} <-> {}", name, local.candidate(), remote.candidate()
      );
  });

  return pc;
}

std::string
WebRtcSession::videoOfferSdp(const rtc::Description &local, const VideoConfig &config) const {
  std::string sdp = local.generateSdp("\r\n");
  VideoSdpParams params = config.bitrates;
  params.preferredProfile = config.profile;
  sdp = mungeVideoOffer(sdp, params);
  sdp += cloudGamingSuffix(config.fps, config.streamWidth, config.streamHeight, config.rgbRange);
  return sdp;
}

void WebRtcSession::createVideo(const VideoConfig &config) {
  closeVideo();

  Peer peer;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    peer.generation = ++generation_;
  }
  peer.pc = makePeerConnection(proto::ST_VIDEO, peer.generation);

  // Video, recvonly, H.264 profiles as Chrome offers them + RTX
  rtc::Description::Video video("0", rtc::Description::Direction::RecvOnly);
  for (const auto &o : H264_OFFERS) {
    video.addH264Codec(
      o.pt,
      std::string("level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=") +
        o.profileLevelId
    );
    video.addRtxCodec(o.rtxPt, o.pt, 90000);
  }
  peer.videoTrack = peer.pc->addTrack(video);
  peer.videoTrack->setMediaHandler(std::make_shared<rtc::H264RtpDepacketizer>());
  peer.videoTrack->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
  peer.videoTrack->chainMediaHandler(std::make_shared<RtpLossCounter>(
    [this] {
      if (onVideoLoss)
        onVideoLoss();
    },
    [this](uint64_t received, uint64_t lost) {
      rtpReceived_ += received;
      rtpLost_ += lost;
    }
  ));
  peer.videoTrack->onFrame([this,
                            generation = peer.generation](rtc::binary data, rtc::FrameInfo info) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (video_.generation != generation)
        return;
    }
    videoBytes_ += data.size();
    if (onVideoFrame)
      onVideoFrame(data, info.timestamp);
  });

  // Audio, recvonly, Opus (no microphone track: §6.1 alternative branch)
  rtc::Description::Audio audio("1", rtc::Description::Direction::RecvOnly);
  audio.addOpusCodec(OPUS_PT);
  peer.audioTrack = peer.pc->addTrack(audio);
  peer.audioTrack->setMediaHandler(std::make_shared<rtc::OpusRtpDepacketizer>());
  peer.audioTrack->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
  peer.audioTrack->onFrame([this,
                            generation = peer.generation](rtc::binary data, rtc::FrameInfo info) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (video_.generation != generation)
        return;
    }
    audioBytes_ += data.size();
    if (onAudioFrame)
      onAudioFrame(data, info.timestamp);
  });

  {
    std::lock_guard<std::mutex> lock(mutex_);
    video_ = peer;
    videoConnected_ = false;
    videoBytes_ = 0;
    audioBytes_ = 0;
    rtpReceived_ = 0;
    rtpLost_ = 0;
  }

  peer.pc->onLocalDescription([this, config, generation = peer.generation](rtc::Description desc) {
    if (desc.type() != rtc::Description::Type::Offer)
      return;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (video_.generation != generation)
        return;
    }
    std::string sdp = videoOfferSdp(desc, config);
    brls::Logger::debug("VideoAndAudioStream: offer\n{}", sdp);
    if (onOffer)
      onOffer(proto::ST_VIDEO, sdp);
  });
  peer.pc->setLocalDescription(rtc::Description::Type::Offer);
}

void WebRtcSession::createInputs() {
  closeInputs();

  Peer peer;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    peer.generation = ++generation_;
  }
  peer.pc = makePeerConnection(proto::ST_INPUTS, peer.generation);

  // §6.1: createDataChannel("InputStream", { ordered: false, maxPacketLifeTime: 1 })
  rtc::DataChannelInit init;
  init.reliability.unordered = true;
  init.reliability.maxPacketLifeTime = std::chrono::milliseconds(1);
  peer.dataChannel = peer.pc->createDataChannel(DATA_CHANNEL_LABEL, init);

  auto generation = peer.generation;
  auto alive = [this, generation] {
    std::lock_guard<std::mutex> lock(mutex_);
    return inputs_.generation == generation;
  };
  peer.dataChannel->onOpen([this, alive] {
    brls::Logger::info("InputStream: data channel open");
    if (alive() && onDataChannelOpen)
      onDataChannelOpen();
  });
  peer.dataChannel->onClosed([this, alive] {
    brls::Logger::info("InputStream: data channel closed");
    if (alive() && onDataChannelClosed)
      onDataChannelClosed("closed");
  });
  peer.dataChannel->onError([this, alive](std::string error) {
    brls::Logger::error("InputStream: data channel error: {}", error);
    if (alive() && onDataChannelClosed)
      onDataChannelClosed(error);
  });
  peer.dataChannel->onMessage(
    [this, alive](rtc::binary data) {
      if (alive() && onDataChannelMessage)
        onDataChannelMessage(data);
    },
    [](std::string text) {
      brls::Logger::warning("InputStream: unexpected text message ({} bytes)", text.size());
    }
  );

  {
    std::lock_guard<std::mutex> lock(mutex_);
    inputs_ = peer;
    inputsConnected_ = false;
  }

  peer.pc->onLocalDescription([this, generation](rtc::Description desc) {
    if (desc.type() != rtc::Description::Type::Offer)
      return;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (inputs_.generation != generation)
        return;
    }
    std::string sdp = desc.generateSdp("\r\n");
    brls::Logger::debug("InputStream: offer\n{}", sdp);
    if (onOffer)
      onOffer(proto::ST_INPUTS, sdp);
  });
  peer.pc->setLocalDescription(rtc::Description::Type::Offer);
}

void WebRtcSession::setRemoteAnswer(StreamType type, const std::string &sdp) {
  std::shared_ptr<rtc::PeerConnection> pc;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pc = type == proto::ST_VIDEO ? video_.pc : inputs_.pc;
  }
  if (!pc) {
    brls::Logger::warning(
      "WebRtc: answer for stream {} without a peer connection", static_cast<int>(type)
    );
    return;
  }
  try {
    pc->setRemoteDescription(rtc::Description(sdp, rtc::Description::Type::Answer));
  } catch (const std::exception &e) {
    brls::Logger::error("WebRtc: setRemoteDescription failed: {}", e.what());
  }
}

void WebRtcSession::addRemoteCandidate(StreamType type, const std::string &iceJson) {
  std::shared_ptr<rtc::PeerConnection> pc;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pc = type == proto::ST_VIDEO ? video_.pc : inputs_.pc;
  }
  if (!pc)
    return;
  try {
    auto j = nlohmann::json::parse(iceJson);
    std::string candidate = j.value("candidate", "");
    std::string mid = j.value("sdpMid", "");
    if (mid.empty() && j.contains("sdpMLineIndex") && j["sdpMLineIndex"].is_number())
      mid = std::to_string(j["sdpMLineIndex"].get<int>());
    if (candidate.empty())
      return; // end-of-candidates marker
    pc->addRemoteCandidate(rtc::Candidate(candidate, mid));
  } catch (const std::exception &e) {
    // §6.1: errors are only logged
    brls::Logger::warning("WebRtc: addIceCandidate failed: {} ({})", e.what(), iceJson);
  }
}

// Detaches and closes a peer; callbacks are reset first so nothing fires into
// the owner while it tears down.
static void closePeer(std::mutex &mutex, WebRtcSession::PeerHandles &slot) {
  WebRtcSession::PeerHandles peer;
  {
    std::lock_guard<std::mutex> lock(mutex);
    peer.swap(slot);
  }
  if (peer.dataChannel) {
    peer.dataChannel->resetCallbacks();
    peer.dataChannel->close();
  }
  if (peer.videoTrack)
    peer.videoTrack->resetCallbacks();
  if (peer.audioTrack)
    peer.audioTrack->resetCallbacks();
  if (peer.pc) {
    peer.pc->resetCallbacks();
    peer.pc->close();
  }
}

void WebRtcSession::closeVideo() {
  videoConnected_ = false;
  closePeer(mutex_, video_);
}

void WebRtcSession::closeInputs() {
  inputsConnected_ = false;
  closePeer(mutex_, inputs_);
}

void WebRtcSession::closeAll() {
  closeVideo();
  closeInputs();
}

bool WebRtcSession::isDataChannelOpen() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return inputs_.dataChannel && inputs_.dataChannel->isOpen();
}

size_t WebRtcSession::dataChannelBufferedAmount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return inputs_.dataChannel ? inputs_.dataChannel->bufferedAmount() : 0;
}

bool WebRtcSession::sendInput(const uint8_t *data, size_t size) {
  std::shared_ptr<rtc::DataChannel> dc;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    dc = inputs_.dataChannel;
  }
  if (!dc || !dc->isOpen())
    return false;
  try {
    return dc->send(reinterpret_cast<const std::byte *>(data), size);
  } catch (const std::exception &e) {
    brls::Logger::warning("InputStream: send failed: {}", e.what());
    return false;
  }
}

void WebRtcSession::requestKeyframe() {
  std::shared_ptr<rtc::Track> track;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    track = video_.videoTrack;
  }
  if (track && track->isOpen()) {
    keyframeRequests_++;
    track->requestKeyframe();
  }
}

WebRtcSession::Stats WebRtcSession::stats() const {
  Stats s;
  std::shared_ptr<rtc::PeerConnection> video, inputs;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    video = video_.pc;
    inputs = inputs_.pc;
  }
  s.videoBytesReceived = videoBytes_;
  s.audioBytesReceived = audioBytes_;
  s.rtpPacketsReceived = rtpReceived_;
  s.rtpPacketsLost = rtpLost_;
  s.keyframeRequests = keyframeRequests_;
  if (video) {
    s.videoRtt = video->rtt();
    rtc::Candidate local, remote;
    if (video->getSelectedCandidatePair(&local, &remote)) {
      s.videoLocalCandidate = local.candidate();
      s.videoRemoteCandidate = remote.candidate();
    }
  }
  if (inputs)
    s.inputsBytesSent = inputs->bytesSent();
  return s;
}

} // namespace vkpcnx::stream
