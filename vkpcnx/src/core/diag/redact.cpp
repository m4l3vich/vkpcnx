#include "core/diag/redact.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>

// Hand-written scanners rather than std::regex: this runs on every log line
// (SDP dumps included) from whatever thread logs, and libstdc++'s regex
// recurses per character — a real risk on the Switch's small thread stacks.

namespace vkpcnx::diag {

namespace {

bool isIdentChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
}

bool isHex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isAlnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }

bool isValueDelim(char c) {
  switch (c) {
  case ' ':
  case '\t':
  case '\r':
  case '\n':
  case '"':
  case '\'':
  case '\\':
  case '&':
  case ',':
  case ';':
  case ')':
  case '(':
  case '}':
  case ']':
  case '<':
  case '>':
    return true;
  default:
    return false;
  }
}

std::string lower(std::string_view s) {
  std::string out(s);
  for (char &c : out)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool endsWith(const std::string &s, const char *suffix) {
  size_t n = std::char_traits<char>::length(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

constexpr size_t kMinSecretLength = 6;

// key=value, "key": "value", key: value (protobuf text), Authorization: Bearer x
std::string redactKeyValues(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  const size_t n = s.size();
  size_t i = 0;
  while (i < n) {
    if (!isIdentChar(s[i]) || (i > 0 && isIdentChar(s[i - 1]))) {
      out += s[i++];
      continue;
    }
    size_t j = i;
    while (j < n && isIdentChar(s[j]))
      j++;
    if (isSecretKey(s.substr(i, j - i))) {
      size_t k = j;
      while (k < n && (s[k] == '"' || s[k] == '\'' || s[k] == '\\'))
        k++; // closing quote of the key, possibly escaped
      while (k < n && s[k] == ' ')
        k++;
      if (k < n && (s[k] == '=' || s[k] == ':')) {
        k++;
        while (k < n && (s[k] == ' ' || s[k] == '"' || s[k] == '\'' || s[k] == '\\'))
          k++;
        size_t valueStart = k, valueEnd = k;
        while (valueEnd < n && !isValueDelim(s[valueEnd]))
          valueEnd++;
        std::string scheme = lower(s.substr(valueStart, valueEnd - valueStart));
        if ((scheme == "bearer" || scheme == "basic") && valueEnd < n && s[valueEnd] == ' ') {
          valueStart = ++valueEnd;
          while (valueEnd < n && !isValueDelim(s[valueEnd]))
            valueEnd++;
        }
        if (valueEnd - valueStart >= kMinSecretLength) {
          out.append(s.substr(i, valueStart - i));
          out += "***";
          i = valueEnd;
          continue;
        }
      }
    }
    out.append(s.substr(i, j - i));
    i = j;
  }
  return out;
}

bool isPrivateIpv4(const int o[4]) {
  return o[0] == 0 || o[0] == 10 || o[0] == 127 || (o[0] == 169 && o[1] == 254) ||
         (o[0] == 172 && o[1] >= 16 && o[1] <= 31) || (o[0] == 192 && o[1] == 168) ||
         (o[0] == 100 && o[1] >= 64 && o[1] <= 127) || o[0] >= 224;
}

std::string maskIpv4(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  const size_t n = s.size();
  size_t i = 0;
  while (i < n) {
    if (!isDigit(s[i]) || (i > 0 && (isAlnum(s[i - 1]) || s[i - 1] == '.'))) {
      out += s[i++];
      continue;
    }
    int octets[4];
    size_t starts[4];
    size_t k = i;
    int count = 0;
    while (count < 4) {
      size_t digits = 0;
      int value = 0;
      starts[count] = k;
      while (k < n && isDigit(s[k]) && digits < 4) {
        value = value * 10 + (s[k] - '0');
        k++;
        digits++;
      }
      if (digits == 0 || digits > 3 || value > 255)
        break;
      octets[count++] = value;
      if (count < 4) {
        if (k >= n || s[k] != '.')
          break;
        k++;
      }
    }
    bool trailingOk = k >= n || (!isAlnum(s[k]) && !(s[k] == '.' && k + 1 < n && isDigit(s[k + 1])));
    if (count == 4 && trailingOk) {
      if (isPrivateIpv4(octets))
        out.append(s, i, k - i);
      else {
        out.append(s, i, starts[3] - i);
        out += 'x';
      }
      i = k;
      continue;
    }
    // Not an address: copy the digit run so the scan doesn't restart inside it
    while (i < n && (isDigit(s[i]) || s[i] == '.'))
      out += s[i++];
  }
  return out;
}

std::string maskIpv6(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  const size_t n = s.size();
  size_t i = 0;
  while (i < n) {
    bool startable = isHex(s[i]) || s[i] == ':';
    if (!startable || (i > 0 && (isAlnum(s[i - 1]) || s[i - 1] == ':' || s[i - 1] == '.'))) {
      out += s[i++];
      continue;
    }
    size_t k = i;
    while (k < n && (isHex(s[k]) || s[k] == ':'))
      k++;
    std::string_view run(s.data() + i, k - i);
    size_t colons = 0, hexDigits = 0, group = 0, maxGroup = 0;
    for (char c : run) {
      if (c == ':') {
        colons++;
        group = 0;
      } else {
        hexDigits++;
        maxGroup = std::max(maxGroup, ++group);
      }
    }
    bool doubleColon = run.find("::") != std::string_view::npos;
    bool boundary = k >= n || !isAlnum(s[k]);
    bool address = boundary && colons >= 2 && colons <= 7 && (doubleColon || colons == 7) &&
                   maxGroup <= 4 && hexDigits >= 4;
    if (!address) {
      out.append(run);
      i = k;
      continue;
    }
    std::string first = lower(run.substr(0, run.find(':')));
    bool local = first.empty() || // ::1, ::ffff:…
                 (first.size() == 4 && first.compare(0, 3, "fe8") >= 0 &&
                  first.compare(0, 3, "feb") <= 0) ||
                 (first.size() == 4 && (first[0] == 'f' && (first[1] == 'c' || first[1] == 'd')));
    if (local) {
      out.append(run);
    } else {
      // keep the /48 (three groups), or everything up to an earlier "::"
      size_t cut = 0, seen = 0;
      for (size_t p = 0; p < run.size(); p++) {
        if (run[p] != ':')
          continue;
        if (p + 1 < run.size() && run[p + 1] == ':') {
          cut = p + 2;
          break;
        }
        if (++seen == 3) {
          cut = p + 1;
          break;
        }
      }
      out.append(run.substr(0, cut));
      out += 'x';
    }
    i = k;
  }
  return out;
}

std::mutex homeMutex;
std::string homeDir;

} // namespace

bool isSecretKey(std::string_view key) {
  static const char *const exact[] = {
    "code",
    "oauth2_code",
    "oauth_code",
    "auth_code",
    "password",
    "passwd",
    "secret",
    "cookie",
    "set-cookie",
    "authorization",
    "ice-pwd",
    "session_key",
    "sessionkey",
    "api_key",
    "apikey",
  };
  std::string k = lower(key);
  for (const char *e : exact)
    if (k == e)
      return true;
  return endsWith(k, "token") || endsWith(k, "password") || endsWith(k, "secret");
}

void setRedactedHome(const std::string &home) {
  std::lock_guard<std::mutex> lock(homeMutex);
  homeDir = home.size() >= 3 ? home : std::string();
}

// Avatar URLs carry the account's user id
static void maskAvatarIds(std::string &s) {
  static const std::string marker = "/avatar/";
  for (size_t pos = s.find(marker); pos != std::string::npos; pos = s.find(marker, pos + 1)) {
    size_t start = pos + marker.size(), end = start;
    while (end < s.size() && isDigit(s[end]))
      end++;
    if (end > start)
      s.replace(start, end - start, "***");
  }
}

std::string redact(std::string_view text) {
  std::string s = maskIpv6(maskIpv4(redactKeyValues(text)));
  maskAvatarIds(s);
  std::lock_guard<std::mutex> lock(homeMutex);
  if (!homeDir.empty())
    for (size_t pos = s.find(homeDir); pos != std::string::npos; pos = s.find(homeDir, pos + 1))
      s.replace(pos, homeDir.size(), "~");
  return s;
}

} // namespace vkpcnx::diag
