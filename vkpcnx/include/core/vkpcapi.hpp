#pragma once
#include <nlohmann/json.hpp>
#include <stdexcept>

class VKPCAPI {
public:
  using QueryParams = std::map<std::string, std::string>;

  struct Exception : public std::runtime_error {
    using std::runtime_error::runtime_error;
  };

  struct NotLoggedInException : public Exception {
    NotLoggedInException() : Exception("Not logged in yet") {}
  };

  // Non-2xx reply (after token refresh); `body` is the parsed error JSON if any
  struct ApiException : public Exception {
    ApiException(long status, nlohmann::json body, const std::string &what)
      : Exception(what), status(status), body(std::move(body)) {}
    long status;
    nlohmann::json body;
    // "error" / "detail" / "message" field of the body, or the HTTP status
    std::string userMessage() const;
  };

  static nlohmann::json usersMe() { return performGet("/users/me"); }
  static nlohmann::json queueServerLoad() { return performGet("/queue/server_load"); }
  static nlohmann::json indexGames(QueryParams params) {
    return performGet("/games?" + buildQueryString(params));
  }

  // Play session (protocol doc §2)
  // Enter the queue for a game launcher; response has status/number/is_ping_ready
  static nlohmann::json enterQueue(int launcherId) {
    return performPost("/game_launchers/" + std::to_string(launcherId) + "/queue", nlohmann::json::object());
  }
  // Poll the queue until status == "allowed"
  static nlohmann::json getQueue() { return performGet("/queue"); }
  static nlohmann::json leaveQueue() { return performDelete("/queue/delete"); }
  // play_url for a ping-test session (work-mode ping_test)
  static nlohmann::json pingTest() { return performPost("/pingtest", {{"type", "webrtc_client"}}); }
  // play_url for the real session
  static nlohmann::json runLauncher(int launcherId, bool demo = false) {
    nlohmann::json body = {{"type", "webrtc_client"}};
    if (demo)
      body["is_demo_launch"] = true;
    return performPost("/game_launchers/" + std::to_string(launcherId) + "/run/", body);
  }
  static nlohmann::json forceKillSessions(int launcherId) {
    return performPost(
      "/game_launchers/" + std::to_string(launcherId) + "/force_kill_sessions", nlohmann::json::object()
    );
  }
  static nlohmann::json activeSessions() { return performGet("/game_sessions/active"); }

private:
  static nlohmann::json performPost(const std::string &endpoint, const nlohmann::json &body);
  static nlohmann::json performGet(const std::string &endpoint);
  static nlohmann::json performDelete(const std::string &endpoint);
  static bool tryRefreshToken();

  static std::string buildQueryString(const QueryParams &params);
};