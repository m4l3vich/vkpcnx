#pragma once
#include <string>

namespace vkpcnx::utils {
// Path of the bundled CA certificate file (resources/cacert.pem), or "" when
// it isn't shipped. Needed for TLS verification under mbedTLS (Switch), where
// there is no system certificate store.
std::string caBundlePath();
// True where the TLS library can verify against an OS certificate store.
bool hasSystemCaStore();
// Development aid: VKPCNX_INSECURE_TLS=1 disables certificate verification on
// the signalling sockets (for local mock servers with self-signed certs).
bool allowInsecureTls();
} // namespace vkpcnx::utils
