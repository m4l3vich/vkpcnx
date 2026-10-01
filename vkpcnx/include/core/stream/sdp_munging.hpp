#pragma once

#include <string>
#include <vector>

namespace vkpcnx::stream {

// H.264 profiles the web player knows, by profile-level-id (§6.2 (a))
enum class H264Profile { Auto, High, ConstrainedHigh, Main, Base, ConstrainedBase };

const char *h264ProfileLevelId(H264Profile profile);       // "64001f" …
H264Profile h264ProfileFromLevelId(const std::string &id); // Auto if unknown

struct VideoSdpParams {
  H264Profile preferredProfile = H264Profile::Auto; // Auto ⇒ High
  int startKbps = 8000;
  int minKbps = 1000;
  int maxKbps = 25000;
};

// Applies the web player's offer rewriting (§6.2 a–c) to a video offer SDP:
//  (a) H.264 payload types reordered so the preferred profile comes first
//  (b) x-google-*-bitrate / sps-pps-idr-in-keyframe fmtp lines added
//  (c) rtx-time=125 appended to every apt= fmtp
std::string mungeVideoOffer(const std::string &sdp, const VideoSdpParams &params);

// (d) the server-side hint appended after setLocalDescription;
// fps 0 = auto, width/height 0 = auto, rgbRange 0 auto / 1 limited / 2 full
std::string cloudGamingSuffix(int fps, int streamWidth, int streamHeight, int rgbRange);

// Bitrate defaults of §6.3 (Mbit → kbps done here)
struct BitrateDefaults {
  int startKbps;
  int minKbps;
  int maxKbps;
};
BitrateDefaults defaultBitrates(int streamWidth, int streamHeight, int fps);

// §6.3 resolution list; snaps a monitor size to the first entry ≥ size − 32
std::pair<int, int> snapStreamResolution(int width, int height);

} // namespace vkpcnx::stream
