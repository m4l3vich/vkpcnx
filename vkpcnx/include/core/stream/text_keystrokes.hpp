#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vkpcnx::stream {

// One key press needed to type a character on the server: the DirectInput
// scan code (§9.1), whether Shift is held, and the keyboard layout the VM must
// be switched to for that key to produce the character.
struct Keystroke {
  uint8_t dik = 0;
  bool shift = false;
  const char *locale = "en-US"; // "en-US" | "ru-RU"
};

// Turns UTF-8 text (e.g. from the on-screen keyboard) into the sequence of key
// presses that types it on a US / ЙЦУКЕН layout. Latin letters, digits, ASCII
// punctuation, space, Tab and newline (Enter) use en-US; Cyrillic and № use
// ru-RU. Characters neither layout can type are skipped.
std::vector<Keystroke> textToKeystrokes(const std::string &utf8);

} // namespace vkpcnx::stream
