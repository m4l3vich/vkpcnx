#include "core/stream/play_url.hpp"

#include <cstdlib>
#include <map>

namespace vkpcnx::stream {

std::string urlDecode(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) &&
        isxdigit((unsigned char)s[i + 2])) {
      out.push_back(static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16)));
      i += 2;
    } else if (s[i] == '+') {
      out.push_back(' ');
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

static int64_t toInt(const std::string &s, int64_t def = 0) {
  if (s.empty())
    return def;
  char *end = nullptr;
  long long v = std::strtoll(s.c_str(), &end, 10);
  return end == s.c_str() ? def : v;
}

std::optional<PlayUrl> PlayUrl::parse(const std::string &url) {
  static const std::string scheme = "playkey:";
  if (url.compare(0, scheme.size(), scheme) != 0)
    return std::nullopt;

  size_t q = url.find('?');
  if (q == std::string::npos)
    return std::nullopt;

  std::map<std::string, std::string> params;
  std::string query = url.substr(q + 1);
  size_t pos = 0;
  while (pos <= query.size()) {
    size_t amp = query.find('&', pos);
    std::string kv = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
    if (!kv.empty()) {
      size_t eq = kv.find('=');
      if (eq == std::string::npos)
        params[urlDecode(kv)] = "";
      else
        params[urlDecode(kv.substr(0, eq))] = urlDecode(kv.substr(eq + 1));
    }
    if (amp == std::string::npos)
      break;
    pos = amp + 1;
  }

  auto get = [&](const char *key) -> std::string {
    auto it = params.find(key);
    return it == params.end() ? std::string() : it->second;
  };

  PlayUrl p;
  p.host = get("host");
  p.port = static_cast<int>(toInt(get("port")));
  if (p.host.empty() || p.port <= 0)
    return std::nullopt;

  p.token = get("token");
  p.gameId = toInt(get("gameId"));
  p.sessionId = get("session-id");
  if (!p.sessionId.empty()) {
    // "c123" → play_token_id 123; a bare number is used as-is
    p.playTokenId = toInt(p.sessionId[0] == 'c' ? p.sessionId.substr(1) : p.sessionId);
  }
  p.language = get("language");
  p.uid = get("uid");
  p.fps = static_cast<int>(toInt(get("fps")));
  std::string res = get("resolution");
  size_t x = res.find('x');
  if (x != std::string::npos) {
    p.resolutionWidth = static_cast<int>(toInt(res.substr(0, x)));
    p.resolutionHeight = static_cast<int>(toInt(res.substr(x + 1)));
  }
  p.workMode = static_cast<WorkMode>(toInt(get("work-mode")));
  p.exeCmdLine = get("exe-cmd-line");
  p.gcsettings = get("gcsettings");
  p.regionCode = get("region-code");
  p.allowFeatures = static_cast<uint32_t>(toInt(get("allow-features")));
  p.username = get("username");
  p.password = get("password");
  return p;
}

std::string PlayUrl::redact(const std::string &url) {
  static const char *secretKeys[] = {"token", "password"};

  std::string out;
  size_t q = url.find('?');
  if (q == std::string::npos)
    return url;
  out = url.substr(0, q + 1);

  std::string query = url.substr(q + 1);
  size_t pos = 0;
  while (pos <= query.size()) {
    size_t amp = query.find('&', pos);
    std::string kv = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);

    size_t eq = kv.find('=');
    bool isSecret = false;
    if (eq != std::string::npos) {
      std::string key = kv.substr(0, eq);
      for (const char *secretKey : secretKeys)
        isSecret = isSecret || key == secretKey;
    }
    out += isSecret ? kv.substr(0, eq + 1) + "***" : kv;

    if (amp == std::string::npos)
      break;
    out += '&';
    pos = amp + 1;
  }
  return out;
}

} // namespace vkpcnx::stream
