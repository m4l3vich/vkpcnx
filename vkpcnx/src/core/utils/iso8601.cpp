#include <cstdio>
#include <cstring>
#include <ctime>
#include <optional>
#include <string>

#if defined(_WIN32)
#define timegm _mkgmtime
#elif defined(__SWITCH__)
// newlib has no timegm(); days-from-civil (Howard Hinnant) is exact for the
// proleptic Gregorian calendar
static std::time_t timegm(std::tm *tm) {
  int y = tm->tm_year + 1900, m = tm->tm_mon + 1, d = tm->tm_mday;
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long long days = static_cast<long long>(era) * 146097 + static_cast<long long>(doe) - 719468;
  return static_cast<std::time_t>(days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}
#endif

namespace vkpcnx::utils {
std::optional<std::string>
Iso8601ToLocal(const std::string &iso, const char *format = "%Y-%m-%d %H:%M:%S") {
  std::tm tm{};
  int offsetSign = 0;
  int offsetHours = 0;
  int offsetMinutes = 0;
  char offsetSignChar = 0;

  int consumed = 0;
  int n = std::sscanf(
    iso.c_str(),
    "%d-%d-%dT%d:%d:%d%n",
    &tm.tm_year,
    &tm.tm_mon,
    &tm.tm_mday,
    &tm.tm_hour,
    &tm.tm_min,
    &tm.tm_sec,
    &consumed
  );
  if (n != 6) {
    return std::nullopt;
  }
  tm.tm_year -= 1900;
  tm.tm_mon -= 1;

  std::string rest = iso.substr(consumed);
  if (!rest.empty() && rest[0] == '.') {
    size_t fracEnd = rest.find_first_not_of("0123456789", 1);
    rest = (fracEnd == std::string::npos) ? "" : rest.substr(fracEnd);
  }
  if (!rest.empty() && (rest[0] == 'Z' || rest[0] == 'z')) {
    // UTC, offset stays zero.
  } else if (!rest.empty() &&
             std::sscanf(rest.c_str(), "%c%d:%d", &offsetSignChar, &offsetHours, &offsetMinutes) ==
               3) {
    offsetSign = (offsetSignChar == '-') ? -1 : 1;
  } else if (!rest.empty()) {
    return std::nullopt; // Unrecognized suffix.
  }
  // No suffix at all is treated as already-UTC, same as 'Z'.

  std::time_t utcTime = timegm(&tm);
  if (utcTime == static_cast<std::time_t>(-1)) {
    return std::nullopt;
  }
  utcTime -= offsetSign * (offsetHours * 3600 + offsetMinutes * 60);

  std::tm localTm{};
#if defined(_WIN32)
  localtime_s(&localTm, &utcTime);
#else
  localtime_r(&utcTime, &localTm);
#endif

  char buf[64];
  if (std::strftime(buf, sizeof(buf), format, &localTm) == 0) {
    return std::nullopt;
  }
  return std::string(buf);
}
} // namespace vkpcnx::utils