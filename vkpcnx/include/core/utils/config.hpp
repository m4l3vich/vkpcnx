#pragma once
#include <string>
#include <string_view>

namespace vkpcnx::utils {
inline const std::string AUTH_API_BASE = "https://api.cloud.vkplay.ru";
inline const std::string PAIRING_QR_LINK_PREFIX = "https://cloud.vkplay.ru/tv?code=";
inline const std::string OAUTH_TOKEN_ENDPOINT =
  "https://o2-ext-ac.vkplay.ru/api/v3/pub/oauth2/token";
inline const std::string CLOUD_REST_API_BASE = "https://userapi.cloud.vkplay.ru/api";
inline const std::string REFRESH_TOKEN_ENDPOINT = "https://o2-ac.vkplay.ru/token";
inline constexpr std::string_view AVATAR_URL_FMT = "https://avatar.vkplay.ru/avatar/{}.jpeg";
inline const std::string OAUTH_CLIENT_ID = "ggljghapocutrxffpcdvhbdiabmqustr";
inline constexpr int AUTH_PULL_TOKEN_LIFETIME = 1800; // seconds
} // namespace vkpcnx::utils
