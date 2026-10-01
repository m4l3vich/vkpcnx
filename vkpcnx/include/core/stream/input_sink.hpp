#pragma once

#include <cstdint>
#include <string>

namespace vkpcnx::stream {

// Where StreamActivity sends the input it collects (protocol doc §9): the
// session's InputChannel, or the controls playground's local desktop
// (view/playground_desktop.hpp). Constants are the pk:: ones of
// input_encoder.hpp. Called on the UI thread.
class InputSink {
public:
  virtual ~InputSink() = default;

  // Focus / pointer lock: losing focus releases everything (§8.6)
  virtual void setFocus(bool focused) = 0;
  // Current lock-key state (bits of pk::LOCK_*)
  virtual void setLockKeys(uint8_t lockKeys) = 0;

  // `locale` is "en-US"/"ru-RU"/"" (keep previous)
  virtual void keyEvent(uint8_t dik, bool pressed, const std::string &locale = {}) = 0;
  virtual void mouseMove(int dx, int dy) = 0;
  // Jump the absolute position (video-rect space of §7.3) to x/y
  virtual void mouseMoveTo(int x, int y) = 0;
  virtual void mouseButton(uint8_t button, bool pressed) = 0;
  virtual void mouseWheel(int steps) = 0; // ±1 per notch
  virtual void gamepadButton(int index, uint8_t button, bool pressed) = 0;
  virtual void gamepadAxis(int index, uint8_t axis, int32_t value) = 0;
  virtual void gamepadPov(int index, uint32_t pov) = 0;
  // "release everything" for the last gamepad disconnecting etc.
  virtual void releaseAll() = 0;

  // Client → server clipboard text (§7.7)
  virtual void sendClipboardText(const std::string &text) = 0;
};

} // namespace vkpcnx::stream
