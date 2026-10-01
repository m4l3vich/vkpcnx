#include "core/stream/sdp_munging.hpp"

#include <algorithm>
#include <map>
#include <sstream>

namespace vkpcnx::stream {

const char *h264ProfileLevelId(H264Profile profile) {
  switch (profile) {
  case H264Profile::ConstrainedHigh:
    return "640c1f";
  case H264Profile::Main:
    return "4d001f";
  case H264Profile::Base:
    return "42001f";
  case H264Profile::ConstrainedBase:
    return "42e01f";
  case H264Profile::High:
  case H264Profile::Auto:
  default:
    return "64001f";
  }
}

static std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

H264Profile h264ProfileFromLevelId(const std::string &id) {
  std::string l = lower(id);
  if (l == "64001f")
    return H264Profile::High;
  if (l == "640c1f")
    return H264Profile::ConstrainedHigh;
  if (l == "4d001f")
    return H264Profile::Main;
  if (l == "42001f")
    return H264Profile::Base;
  if (l == "42e01f")
    return H264Profile::ConstrainedBase;
  return H264Profile::Auto;
}

static std::vector<std::string> splitLines(const std::string &sdp) {
  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos < sdp.size()) {
    size_t nl = sdp.find('\n', pos);
    std::string line = sdp.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    lines.push_back(line);
    if (nl == std::string::npos)
      break;
    pos = nl + 1;
  }
  return lines;
}

static std::vector<std::string> splitWords(const std::string &s) {
  std::vector<std::string> words;
  std::istringstream in(s);
  std::string w;
  while (in >> w)
    words.push_back(w);
  return words;
}

// Parses "a=fmtp:<pt> k=v;k=v" into (pt, params) — returns false if not an fmtp line
static bool
parseFmtp(const std::string &line, int &pt, std::map<std::string, std::string> &params) {
  static const std::string prefix = "a=fmtp:";
  if (line.compare(0, prefix.size(), prefix) != 0)
    return false;
  size_t sp = line.find(' ');
  if (sp == std::string::npos)
    return false;
  pt = std::atoi(line.substr(prefix.size(), sp - prefix.size()).c_str());
  std::string rest = line.substr(sp + 1);
  size_t pos = 0;
  while (pos <= rest.size()) {
    size_t semi = rest.find(';', pos);
    std::string kv = rest.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
    size_t eq = kv.find('=');
    if (eq != std::string::npos)
      params[kv.substr(0, eq)] = kv.substr(eq + 1);
    else if (!kv.empty())
      params[kv] = "";
    if (semi == std::string::npos)
      break;
    pos = semi + 1;
  }
  return true;
}

std::string mungeVideoOffer(const std::string &sdp, const VideoSdpParams &params) {
  auto lines = splitLines(sdp);

  // Collect the H.264 payload types that qualify for reordering: pm=1, laa=1
  struct H264Pt {
    int pt;
    std::string profileLevelId;
    H264Profile profile;
  };
  std::vector<H264Pt> h264;
  for (const auto &line : lines) {
    int pt;
    std::map<std::string, std::string> p;
    if (!parseFmtp(line, pt, p))
      continue;
    auto laa = p.find("level-asymmetry-allowed");
    auto pm = p.find("packetization-mode");
    auto pl = p.find("profile-level-id");
    if (laa == p.end() || pm == p.end() || pl == p.end())
      continue;
    if (laa->second != "1" || pm->second != "1")
      continue;
    h264.push_back({pt, lower(pl->second), h264ProfileFromLevelId(pl->second)});
  }

  H264Profile preferred =
    params.preferredProfile == H264Profile::Auto ? H264Profile::High : params.preferredProfile;
  static const H264Profile knownOrder[] = {
    H264Profile::High,
    H264Profile::ConstrainedHigh,
    H264Profile::Main,
    H264Profile::Base,
    H264Profile::ConstrainedBase,
  };

  std::vector<int> ordered;
  auto pushIfAbsent = [&](int pt) {
    if (std::find(ordered.begin(), ordered.end(), pt) == ordered.end())
      ordered.push_back(pt);
  };
  for (const auto &h : h264)
    if (h.profile == preferred)
      pushIfAbsent(h.pt);
  for (H264Profile prof : knownOrder)
    for (const auto &h : h264)
      if (h.profile == prof)
        pushIfAbsent(h.pt);
  std::vector<H264Pt> unknown;
  for (const auto &h : h264)
    if (h.profile == H264Profile::Auto)
      unknown.push_back(h);
  std::sort(unknown.begin(), unknown.end(), [](const H264Pt &a, const H264Pt &b) {
    return std::strtoul(a.profileLevelId.c_str(), nullptr, 16) >
           std::strtoul(b.profileLevelId.c_str(), nullptr, 16);
  });
  for (const auto &h : unknown)
    pushIfAbsent(h.pt);

  std::string out;
  for (auto &line : lines) {
    if (line.compare(0, 8, "m=video ") == 0) {
      auto words = splitWords(line);
      if (words.size() > 3) {
        std::vector<std::string> rest(words.begin() + 3, words.end());
        std::string rebuilt = words[0] + " " + words[1] + " " + words[2];
        for (int pt : ordered)
          rebuilt += " " + std::to_string(pt);
        for (const auto &w : rest) {
          int pt = std::atoi(w.c_str());
          if (std::find(ordered.begin(), ordered.end(), pt) == ordered.end())
            rebuilt += " " + w;
        }
        line = rebuilt;
      }
      out += line + "\r\n";
      continue;
    }

    int pt;
    std::map<std::string, std::string> p;
    if (parseFmtp(line, pt, p)) {
      // (b) exact Chrome fmtp shape: laa;pm;profile-level-id
      static const std::string laaPrefix = " level-asymmetry-allowed=";
      size_t sp = line.find(' ');
      std::string paramsStr = line.substr(sp);
      if (paramsStr.compare(0, laaPrefix.size(), laaPrefix) == 0 && p.count("packetization-mode") &&
          p.count("profile-level-id") && p.size() == 3) {
        std::string tag = "a=fmtp:" + std::to_string(pt) + " ";
        out += line + "\r\n";
        out += tag + "x-google-start-bitrate=" + std::to_string(params.startKbps) + "\r\n";
        out += tag + "x-google-min-bitrate=" + std::to_string(params.minKbps) + "\r\n";
        out += tag + "x-google-max-bitrate=" + std::to_string(params.maxKbps) + "\r\n";
        out += tag + "sps-pps-idr-in-keyframe=1\r\n";
        continue;
      }
      // (c) rtx
      if (p.size() == 1 && p.count("apt")) {
        out += "a=fmtp:" + std::to_string(pt) + " apt=" + p["apt"] + ";rtx-time=125\r\n";
        continue;
      }
    }
    out += line + "\r\n";
  }
  return out;
}

std::string cloudGamingSuffix(int fps, int streamWidth, int streamHeight, int rgbRange) {
  return "cloudgaming:fps=" + std::to_string(fps) + ";streamwidth=" + std::to_string(streamWidth) +
         ";streamheight=" + std::to_string(streamHeight) +
         ";refframes=1;slices=1;rgbrange=" + std::to_string(rgbRange) + ";";
}

BitrateDefaults defaultBitrates(int streamWidth, int streamHeight, int fps) {
  // §6.3: Mbit values ×1000
  int maxMbit = 25;
  if (streamWidth > 2560 || streamHeight > 1440)
    maxMbit = 40;
  else if (streamWidth > 1920 || streamHeight > 1080)
    maxMbit = 32;
  if (fps > 90)
    maxMbit += 6;
  else if (fps > 60)
    maxMbit += 3;
  maxMbit = std::min(maxMbit, 40);

  // start bitrate by stream width, for fps ≤33 / ≤60 / >60
  static const int startTable[4][3] = {{8, 9, 10}, {12, 15, 17}, {17, 21, 25}, {24, 30, 37}};
  int tier = fps <= 33 ? 0 : fps <= 60 ? 1 : 2;
  int row = streamWidth >= 3800 ? 3 : streamWidth >= 2000 ? 2 : streamWidth >= 1600 ? 1 : 0;
  int startMbit = startTable[row][tier];

  return {startMbit * 1000, 1 * 1000, maxMbit * 1000};
}

std::pair<int, int> snapStreamResolution(int width, int height) {
  static const int sizes[][2] = {
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
  for (const auto &s : sizes)
    if (s[0] >= width - 32 && s[1] >= height - 32)
      return {s[0], s[1]};
  return {4096, 2160};
}

} // namespace vkpcnx::stream
