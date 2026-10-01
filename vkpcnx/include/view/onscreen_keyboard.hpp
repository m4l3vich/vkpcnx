#pragma once

#include <borealis.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace vkpcnx {

// Custom on-screen keyboard drawn over the stream: a PC layout (5 rows of 15
// units) with EN / RU (ЙЦУКЕН) labels and an Fn layer (F-keys, navigation).
// Keys are sent live: a finger or gamepad button going down presses the key
// on the server, going up releases it (autorepeat is the VM's).
//
// Shift / Ctrl / Alt / Win are sticky: a tap latches the modifier until the
// next key is released, a second tap within kLockWindow locks it, another tap
// releases it. Caps toggles the lock (and is sent as a key).
//
// Input is fed by StreamActivity (borealis input is blocked while the stream
// is captured): touches per finger, the gamepad through focus / actions.
class OnscreenKeyboard : public brls::View {
public:
  // Gamepad shortcuts (B, X, Y, L, R, Plus)
  enum class Action { Backspace, Shift, Space, Language, FnLayer, Enter };

  OnscreenKeyboard();

  void draw(
    NVGcontext *vg,
    float x,
    float y,
    float width,
    float height,
    brls::Style style,
    brls::FrameContext *ctx
  ) override;

  // Touch, in window coordinates. touchDown() returns false (and ignores the
  // finger) when the point is outside the keyboard.
  bool touchDown(int finger, brls::Point p);
  void touchMove(int finger, brls::Point p); // sliding off a key releases it
  void touchUp(int finger);

  // Gamepad
  void moveFocus(int dx, int dy);
  void pressFocused(bool down);
  void pressAction(Action action, bool down);

  // Releases every held key and modifier (sending the releases)
  void releaseAll();

  // dik: DirectInput scan code (§9.1); locale "en-US"/"ru-RU" for character
  // keys, "" otherwise
  std::function<void(uint8_t dik, bool pressed, const std::string &locale)> onKey;
  std::function<void(bool on)> onCapsChanged;
  std::function<void()> onClose;
  std::function<void()> onDockToggle;

private:
  enum class Mod { Off, Latched, Locked };
  struct Press {
    int key = -1;   // index into the key table
    uint8_t dik = 0; // what was sent on press (the layer may change meanwhile)
  };
  enum class StripButton { None, Dock, Close };

  // Sources of a press: fingers use their id, the gamepad negative ids
  static constexpr int kSourceFocus = -1;
  static constexpr int kSourceActionBase = -100;

  void keyDown(int source, int key);
  void keyUp(int source);
  void tapModifier(int key);
  void releaseLatched();
  uint8_t dikFor(int key) const; // 0 = nothing in the current layer
  std::string labelFor(int key) const;
  bool shiftActive() const;
  int keyAt(brls::Point p) const; // -1 = none
  StripButton stripButtonAt(brls::Point p) const;
  // brls::View::getFrame() isn't const
  brls::Rect layoutFrame() const { return const_cast<OnscreenKeyboard *>(this)->getFrame(); }
  brls::Rect keyRect(int key) const;
  brls::Rect stripButtonRect(StripButton b) const;

  bool russian_ = false;
  bool fnLayer_ = false;
  bool caps_ = false;
  bool showFocus_ = false;
  int focus_ = 0;
  std::map<int, Mod> mods_; // modifier key index → state
  std::map<int, std::chrono::steady_clock::time_point> latchedAt_;
  std::map<int, Press> presses_;           // source → held key
  std::map<int, StripButton> stripTouches_; // finger → strip button under it
  std::map<int, int> pressCount_;           // key → sources holding it (drawn pressed)
};

} // namespace vkpcnx
