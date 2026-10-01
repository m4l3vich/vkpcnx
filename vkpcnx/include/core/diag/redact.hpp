#pragma once

#include <string>
#include <string_view>

// Scrubs secrets and personal data from log lines before they reach a log
// file or a debug report, so a report can be shared publicly as is:
//   - values of secret-looking keys (token, access_token, password, code,
//     cookie, authorization, ice-pwd, ...) in key=value, "key":"value" and
//     protobuf `key: value` form → ***  (values under 6 chars are kept, so
//     "error code=5" survives)
//   - public IPv4 addresses → last octet masked (a.b.c.x); private, loopback
//     and CGNAT ranges are kept, since they say which interface ICE picked
//   - public IPv6 addresses → only the /48 prefix kept
//   - the user's home directory → ~, the user id in avatar URLs → ***
namespace vkpcnx::diag {

std::string redact(std::string_view text);

// True for key names whose values redact() hides (case-insensitive)
bool isSecretKey(std::string_view key);

// Directory replaced by "~" (set from $HOME / %USERPROFILE% at startup)
void setRedactedHome(const std::string &home);

} // namespace vkpcnx::diag
