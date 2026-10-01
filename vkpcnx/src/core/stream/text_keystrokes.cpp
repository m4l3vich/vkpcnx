#include "core/stream/text_keystrokes.hpp"

#include <cstring>

namespace vkpcnx::stream {

namespace {

constexpr uint8_t kDikEnter = 28, kDikTab = 15, kDikSpace = 57, kDikBackquote = 41;

// The four US rows in DIK order, unshifted and shifted (§9.1 table)
struct Row {
  uint8_t firstDik;
  const char *plain;
  const char *shifted;
};
constexpr Row kUsRows[] = {
  {2, "1234567890-=", "!@#$%^&*()_+"},
  {16, "qwertyuiop[]", "QWERTYUIOP{}"},
  {30, "asdfghjkl;'", "ASDFGHJKL:\""},
  {44, "zxcvbnm,./", "ZXCVBNM<>?"},
};
// Backslash (43) sits between the rows
constexpr uint8_t kDikBackslash = 43;

// Same physical keys on the Russian ЙЦУКЕН layout, lowercase (uppercase is
// Shift + the same key). '.' on Slash (53) is deliberately left out — it's
// typed via en-US instead.
struct RuRow {
  uint8_t firstDik;
  const char32_t *plain;
};
constexpr RuRow kRuRows[] = {
  {16, U"йцукенгшщзхъ"},
  {30, U"фывапролджэ"},
  {44, U"ячсмитьбю"},
};

// Minimal UTF-8 decoder: returns the code point and advances `i`; invalid
// sequences yield U+FFFD and advance by one byte
char32_t nextCodepoint(const std::string &s, size_t &i) {
  auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
  unsigned char c = b(i);
  int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
  if (len == 0 || i + len > s.size()) {
    i++;
    return 0xFFFD;
  }
  char32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
  for (int k = 1; k < len; k++) {
    if ((b(i + k) & 0xC0) != 0x80) {
      i++;
      return 0xFFFD;
    }
    cp = (cp << 6) | (b(i + k) & 0x3F);
  }
  i += len;
  return cp;
}

bool mapAscii(char c, Keystroke &out) {
  out.locale = "en-US";
  out.shift = false;
  switch (c) {
  case ' ':
    out.dik = kDikSpace;
    return true;
  case '\n':
  case '\r':
    out.dik = kDikEnter;
    return true;
  case '\t':
    out.dik = kDikTab;
    return true;
  case '`':
    out.dik = kDikBackquote;
    return true;
  case '~':
    out.dik = kDikBackquote;
    out.shift = true;
    return true;
  case '\\':
    out.dik = kDikBackslash;
    return true;
  case '|':
    out.dik = kDikBackslash;
    out.shift = true;
    return true;
  default:
    break;
  }
  for (const Row &row : kUsRows) {
    if (const char *p = std::strchr(row.plain, c); p && *p) {
      out.dik = static_cast<uint8_t>(row.firstDik + (p - row.plain));
      return true;
    }
    if (const char *p = std::strchr(row.shifted, c); p && *p) {
      out.dik = static_cast<uint8_t>(row.firstDik + (p - row.shifted));
      out.shift = true;
      return true;
    }
  }
  return false;
}

bool mapCyrillic(char32_t cp, Keystroke &out) {
  out.locale = "ru-RU";
  out.shift = false;
  if (cp == U'№') { // Shift+3 on ЙЦУКЕН
    out.dik = 4;
    out.shift = true;
    return true;
  }
  if (cp == U'Ё') {
    cp = U'ё';
    out.shift = true;
  } else if (cp >= U'А' && cp <= U'Я') {
    cp += U'а' - U'А';
    out.shift = true;
  }
  if (cp == U'ё') {
    out.dik = kDikBackquote;
    return true;
  }
  for (const RuRow &row : kRuRows) {
    for (const char32_t *p = row.plain; *p; p++) {
      if (*p == cp) {
        out.dik = static_cast<uint8_t>(row.firstDik + (p - row.plain));
        return true;
      }
    }
  }
  return false;
}

} // namespace

std::vector<Keystroke> textToKeystrokes(const std::string &utf8) {
  std::vector<Keystroke> out;
  size_t i = 0;
  while (i < utf8.size()) {
    char32_t cp = nextCodepoint(utf8, i);
    Keystroke k;
    if (cp < 0x80 ? mapAscii(static_cast<char>(cp), k) : mapCyrillic(cp, k))
      out.push_back(k);
  }
  return out;
}

} // namespace vkpcnx::stream
