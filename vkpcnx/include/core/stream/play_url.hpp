#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace vkpcnx::stream {

// Parsed `play_url` returned by the REST API (protocol doc §2.1):
//   playkey:///?host=…&port=…&token=…[&gameId=…][&session-id=c123][&language=ru]…
struct PlayUrl {
  enum class WorkMode { GameStream = 0, PingTest = 1, ManagerTest = 2, RemotePlay = 3 };
  enum class AuthType { GameToken, SingleToken, UserPassword };

  std::string host;
  int port = 0;
  std::string token;
  int64_t gameId = 0;
  int64_t playTokenId = 0; // from session-id ("c123" → 123)
  std::string sessionId;   // raw session-id value
  std::string language;    // "ru" / "en" / empty
  std::string uid;
  int fps = 0;             // startup fps limit (0 = auto)
  int resolutionWidth = 0; // startup resolution limit (0 = auto)
  int resolutionHeight = 0;
  WorkMode workMode = WorkMode::GameStream;
  std::string exeCmdLine;
  std::string gcsettings;
  std::string regionCode;
  uint32_t allowFeatures = 0; // bit 1 clipboard, 2 smartcards, 4 printers, 8 drives
  std::string username;
  std::string password;

  AuthType authType() const {
    if (!token.empty() && gameId != 0)
      return AuthType::GameToken;
    if (!token.empty())
      return AuthType::SingleToken;
    return AuthType::UserPassword;
  }

  std::string managerUrl() const { return "wss://" + host + ":" + std::to_string(port); }

  // Returns nullopt unless the string is a playkey:// URL with host and port.
  static std::optional<PlayUrl> parse(const std::string &url);

  // Same URL with the token/password query values replaced by "***", safe to log.
  static std::string redact(const std::string &url);
};

std::string urlDecode(const std::string &s);

} // namespace vkpcnx::stream
