#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <rtc/rtc.hpp>

#include "core/stream/sdp_munging.hpp"
#include "protocol_common.pb.h"

namespace vkpcnx::stream {

using StreamType = cg::network::protocol::StreamType;

// The two RTCPeerConnections of protocol doc §6, built on libdatachannel:
//   VideoAndAudioStream (ST_VIDEO)  H.264 video + Opus audio, recvonly
//   InputStream         (ST_INPUTS) one unreliable DataChannel "InputStream"
//
// Offers are generated here (with the §6.2 SDP rewriting applied to the copy
// that goes to the server), answers/candidates are fed back in by the owner.
//
// Thread-safety: all callbacks are invoked from libdatachannel's threads.
// The owner marshals to the UI thread as needed. Public methods may be called
// from any thread.
class WebRtcSession {
public:
  struct VideoConfig {
    H264Profile profile = H264Profile::Auto;
    int fps = 0;         // cloudgaming hint, 0 = auto
    int streamWidth = 0; // 0 = auto
    int streamHeight = 0;
    int rgbRange = 0;
    VideoSdpParams bitrates;
  };

  static constexpr const char *STUN_SERVER = "stun:stun.l.google.com:19302";
  static constexpr const char *DATA_CHANNEL_LABEL = "InputStream";

  WebRtcSession();
  ~WebRtcSession();

  // Offer ready to be sent as CS_SDP_OFFER (already rewritten for the server)
  std::function<void(StreamType type, const std::string &sdp)> onOffer;
  // Local UDP ICE candidate as RTCIceCandidate JSON (CS_ICE_CANDIDATE.ice)
  std::function<void(StreamType type, const std::string &iceJson)> onLocalCandidate;
  std::function<void(StreamType type, rtc::PeerConnection::State state)> onStateChange;
  // Complete H.264 access unit (Annex B) / Opus packet with the RTP timestamp
  std::function<void(const rtc::binary &frame, uint32_t rtpTimestamp)> onVideoFrame;
  std::function<void(const rtc::binary &packet, uint32_t rtpTimestamp)> onAudioFrame;
  // Video RTP packets went missing (called on the WebRTC thread)
  std::function<void()> onVideoLoss;
  std::function<void()> onDataChannelOpen;
  std::function<void(const rtc::binary &message)> onDataChannelMessage;
  std::function<void(const std::string &reason)> onDataChannelClosed;

  // Create the video PC and emit its offer. `reconfigurate` is passed through
  // to the owner via the flag it sends in CS_SDP_OFFER (§6.4).
  void createVideo(const VideoConfig &config);
  // Create the inputs PC + DataChannel and emit its offer.
  void createInputs();

  void setRemoteAnswer(StreamType type, const std::string &sdp);
  void addRemoteCandidate(StreamType type, const std::string &iceJson);

  void closeVideo();
  void closeInputs();
  void closeAll();

  bool isVideoConnected() const { return videoConnected_; }
  bool isInputsConnected() const { return inputsConnected_; }
  bool isDataChannelOpen() const;

  // Send on the input DataChannel; false if closed or over the buffer limit (§7)
  bool sendInput(const uint8_t *data, size_t size);
  size_t dataChannelBufferedAmount() const;

  // Ask the server for a keyframe (PLI) after a decode error
  void requestKeyframe();

  struct Stats {
    size_t videoBytesReceived = 0; // depacketized video payload
    size_t audioBytesReceived = 0;
    size_t inputsBytesSent = 0;
    uint64_t rtpPacketsReceived = 0; // video, cumulative over the session
    uint64_t rtpPacketsLost = 0;     // sequence gaps
    uint64_t keyframeRequests = 0;   // PLIs sent after decode errors
    std::optional<std::chrono::milliseconds> videoRtt;
    std::string videoLocalCandidate, videoRemoteCandidate;
  };
  Stats stats() const;

  static void initLogging();

  struct PeerHandles {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> videoTrack;
    std::shared_ptr<rtc::Track> audioTrack;
    std::shared_ptr<rtc::DataChannel> dataChannel;
    uint32_t generation = 0; // 0 = closed; callbacks of older generations are dropped

    void swap(PeerHandles &o) {
      pc.swap(o.pc);
      videoTrack.swap(o.videoTrack);
      audioTrack.swap(o.audioTrack);
      dataChannel.swap(o.dataChannel);
      std::swap(generation, o.generation);
    }
  };

private:
  using Peer = PeerHandles;

  std::shared_ptr<rtc::PeerConnection> makePeerConnection(StreamType type, uint32_t generation);
  std::string videoOfferSdp(const rtc::Description &local, const VideoConfig &config) const;
  static std::string candidateJson(const rtc::Candidate &c, const std::string &ufrag);

  mutable std::mutex mutex_;
  Peer video_;
  Peer inputs_;
  uint32_t generation_ = 0;
  std::atomic<bool> videoConnected_{false};
  std::atomic<bool> inputsConnected_{false};
  std::atomic<size_t> videoBytes_{0};
  std::atomic<size_t> audioBytes_{0};
  std::atomic<uint64_t> rtpReceived_{0};
  std::atomic<uint64_t> rtpLost_{0};
  std::atomic<uint64_t> keyframeRequests_{0};
};

} // namespace vkpcnx::stream
