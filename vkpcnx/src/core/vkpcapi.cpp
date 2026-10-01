#include "core/vkpcapi.hpp"

#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include "borealis/views/dialog.hpp"
#include "core/http.hpp"
#include "core/settings.hpp"
#include "core/utils/config.hpp"
#include "core/utils/lifecycle.hpp"
#include <curl/curl.h>

static constexpr int MAX_NETWORK_RETRIES = 3;

std::string VKPCAPI::ApiException::userMessage() const {
  if (body.is_object()) {
    for (const char *key : {"error", "detail", "message"}) {
      if (body.contains(key)) {
        const auto &v = body.at(key);
        if (v.is_string())
          return v.get<std::string>();
        return v.dump();
      }
    }
  }
  return "HTTP " + std::to_string(status);
}

nlohmann::json VKPCAPI::performGet(const std::string &endpoint) {
  auto &s = Settings::instance();

  if (!s.has("account"))
    throw VKPCAPI::NotLoggedInException();
  auto accessToken = s.get<std::string>("/account/access_token", "");

  auto headers = Http::Headers{{"access-token", accessToken}};
  Http::Response r;
  for (int attempt = 0; attempt < MAX_NETWORK_RETRIES; attempt++) {
    r = Http::get(vkpcnx::utils::CLOUD_REST_API_BASE + endpoint, headers);
    if (r.status != 0)
      break;
  }

  if (!r.ok()) {
    if (r.status == 403 && tryRefreshToken())
      return performGet(endpoint);
    return nullptr;
  }

  return r.json();
}

nlohmann::json VKPCAPI::performPost(const std::string &endpoint, const nlohmann::json &body) {
  auto &s = Settings::instance();

  if (!s.has("account"))
    throw VKPCAPI::NotLoggedInException();
  auto accessToken = s.get<std::string>("/account/access_token", "");

  auto headers = Http::Headers{{"access-token", accessToken}};
  Http::Response r;
  for (int attempt = 0; attempt < MAX_NETWORK_RETRIES; attempt++) {
    r = Http::postJson(vkpcnx::utils::CLOUD_REST_API_BASE + endpoint, body, headers);
    if (r.status != 0)
      break;
  }

  if (!r.ok()) {
    if (r.status == 403 && tryRefreshToken())
      return performPost(endpoint, body);
    throw ApiException(r.status, r.body.empty() ? nlohmann::json() : r.json(), r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error);
  }

  return r.body.empty() ? nlohmann::json::object() : r.json();
}

nlohmann::json VKPCAPI::performDelete(const std::string &endpoint) {
  auto &s = Settings::instance();

  if (!s.has("account"))
    throw VKPCAPI::NotLoggedInException();
  auto accessToken = s.get<std::string>("/account/access_token", "");

  auto headers = Http::Headers{{"access-token", accessToken}};
  auto r = Http::del(vkpcnx::utils::CLOUD_REST_API_BASE + endpoint, headers);

  if (!r.ok()) {
    if (r.status == 403 && tryRefreshToken())
      return performDelete(endpoint);
    return nullptr;
  }

  return r.body.empty() ? nlohmann::json::object() : r.json();
}

bool VKPCAPI::tryRefreshToken() {
  auto &s = Settings::instance();
  auto refreshToken = s.get<std::string>("/account/refresh_token", "");

  auto body = fmt::format(
    "client_id={}&refresh_token={}&grant_type=refresh_token",
    vkpcnx::utils::OAUTH_CLIENT_ID,
    refreshToken
  );

  auto r =
    Http::post(vkpcnx::utils::REFRESH_TOKEN_ENDPOINT, body, "application/x-www-form-urlencoded");

  if (r.status == 0)
    return tryRefreshToken();
  if (!r.ok()) {
    // TODO: do not disturb if in game?
    auto *dialog = new brls::Dialog(
      "Сессия аккаунта VK Play Cloud истекла, вам нужно пройти авторизацию заново. Нажмите ОК, "
      "чтобы перейти на экран авторизации."
    );
    s.remove("account");
    dialog->addButton("ОК", [] { vkpcnx::utils::relaunch(); });
    dialog->open();

    return false;
  }

  auto json = r.json();
  s.set("/account/access_token", json.at("access_token").get<std::string>());
  s.set("/account/access_token_expires_in", json.at("expires_in").get<std::string>());

  return true;
}

std::string VKPCAPI::buildQueryString(const std::map<std::string, std::string> &params) {
  CURL *curl = curl_easy_init();

  auto urlEncode = [curl](const std::string &value) -> std::string {
    char *output = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.length()));
    if (!output)
      return "";
    std::string encoded(output);
    curl_free(output);
    return encoded;
  };

  std::string result;
  for (auto it = params.begin(); it != params.end(); ++it) {
    if (!result.empty()) {
      result += "&";
    }
    result += urlEncode(it->first) + "=" + urlEncode(it->second);
  }
  return result;
}