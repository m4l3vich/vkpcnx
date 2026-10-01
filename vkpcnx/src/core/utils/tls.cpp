#include "core/utils/tls.hpp"

#include <borealis/core/assets.hpp>
#include <cstdlib>
#include <filesystem>

namespace vkpcnx::utils {

std::string caBundlePath() {
  static const std::string path = [] {
    std::string p = BRLS_ASSET("cacert.pem");
    std::error_code ec;
    return std::filesystem::exists(p, ec) ? p : std::string();
  }();
  return path;
}

bool allowInsecureTls() {
  const char *v = std::getenv("VKPCNX_INSECURE_TLS");
  return v && v[0] == '1';
}

bool hasSystemCaStore() {
#ifdef __SWITCH__
  return false;
#else
  return true;
#endif
}

} // namespace vkpcnx::utils
