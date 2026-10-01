#pragma once

#include <borealis.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <vector>

#include "core/stream/input_sink.hpp"
#include "core/stream/play_url.hpp"
#include "core/stream/stream_session.hpp"
#include "view/game_card.hpp"
#include "view/onscreen_keyboard.hpp"
#include "view/playground_desktop.hpp"
#include "view/stream_overlay.hpp"
#include "view/stream_view.hpp"

// Full-screen streaming: owns the StreamSession, renders video through
// StreamView and forwards keyboard / mouse / gamepad input (protocol doc §9).
//
// Client-side hotkeys (the buttons involved are released on the server):
//  - gamepad L+R+Y (LB+RB+Y), or holding Esc for 3 s: stream overlay panel
//  - gamepad L+R+Minus (LB+RB+Back): toggle the on-screen keyboard (view/onscreen_keyboard.hpp)
//  - gamepad L+R+Plus (LB+RB+Start): toggle gamepad pointer mode
//  - left Ctrl + left Alt (desktop): release the mouse; click to capture again
//
// Overlay "controls" settings (core/settings.hpp, /controls/*):
//  - mapping_mode: literal | positional (A<->B, X<->Y swapped for Joy-Con)
//  - gamepad_pointer: pads drive the mouse instead of the virtual Xbox pad,
//    see pollGamepadPointer()
//  - touchscreen_mode: trackpad | touchscreen, see pollTouch()
//
// While the on-screen keyboard is open the pads drive it instead of the game
// (see pollOskGamepad()) and fingers landing on it type instead of moving the
// mouse.
//
// Controls playground (StreamActivity(Playground{})): no session; the same
// input handling drives a local fake desktop (view/playground_desktop.hpp) so
// the controls settings can be tried out. The labelled B on a gamepad leaves
// it, whatever the mapping mode (instead of the right click of pointer mode),
// unless the on-screen keyboard is open.
class StreamActivity : public brls::Activity {
public:
  StreamActivity(vkpcnx::GameCard::Game game, vkpcnx::stream::PlayUrl playUrl);
  struct Playground {};
  explicit StreamActivity(Playground);
  ~StreamActivity() override;

  CONTENT_FROM_XML_RES("activity/stream.xml");

  void onContentAvailable() override;
  void willAppear(bool resetState) override;
  void onResume() override;
  void willDisappear(bool resetState) override;

private:
  BRLS_BIND(brls::Box, root, "stream/root");
  BRLS_BIND(vkpcnx::StreamView, video, "stream/video");
  BRLS_BIND(brls::Box, overlay, "stream/overlay");
  BRLS_BIND(brls::Label, statusTitle, "stream/status_title");
  BRLS_BIND(brls::Label, statusText, "stream/status_text");
  BRLS_BIND(brls::Box, hudBox, "stream/hud_box");
  BRLS_BIND(brls::Label, hud, "stream/hud");
  BRLS_BIND(brls::Label, timer, "stream/timer");
  BRLS_BIND(brls::Box, oskButton, "stream/osk_button");
  BRLS_BIND(vkpcnx::OnscreenKeyboard, osk, "stream/osk");

  vkpcnx::stream::StreamSession::Config buildConfig() const;
  void setupSession();
  void setupInput();
  // Where input goes: the session's InputChannel or the playground desktop
  bool hasSink() const { return session_ || playground_; }
  vkpcnx::stream::InputSink &sink();
  bool live() const; // streaming, or the playground
  void setCaptured(bool captured);
  void pollInputs(); // once per frame while capturing
  // Client-side gamepad hotkeys, see the class comment
  enum class PadAction { None, Overlay, Keyboard, TogglePointer, Exit };
  PadAction pollGamepads(brls::InputManager *input, float dt);
  void pollGamepadPointer(const brls::ControllerState &st, float dt, PadAction &action);
  // Keyboard open: focus / actions from the pads, right stick = mouse
  void pollOskGamepad(const brls::ControllerState &st, float dt);
  void stickCursor(float x, float y, float dt); // pointer-mode speed and acceleration
  void setPointerButton(uint8_t button, bool pressed);
  void suppressHeldPadButtons(); // held buttons were meant for the client UI
  void releasePad(int index);    // buttons, POV and axes of one virtual pad
  void releasePointerInputs();   // mouse buttons / keys held by the gamepad pointer
  void pollTouch(brls::InputManager *input, float dt);
  void tapClick(); // trackpad tap: left press now, release after the tap-drag window
  // Cursor helpers: video-rect coordinates, also forwarded to the server
  void moveCursorBy(float dx, float dy);
  void moveCursorTo(float x, float y);
  void clickMouse(uint8_t button);
  void onKey(brls::KeyState key);
  void showStreamOverlay();
  void showErrorAndLeave(const std::string &title, const std::string &message);
  void leave();
  void updateHud();
  // A "Stats:" line in the log every kStatsLogInterval while streaming, so a
  // report shows what the connection and decoder did before a problem
  void logStats();
  static constexpr auto kStatsLogInterval = std::chrono::seconds(2);
  std::chrono::steady_clock::time_point statsLoggedAt_{};
  std::optional<vkpcnx::stream::StreamSession::Stats> lastLoggedStats_;
  void setHudVisible(bool visible);
  // On-screen keyboard button: session-only toggle (not persisted), off by
  // default; dragged around with a finger in pollTouch()
  void setOskButtonVisible(bool visible);
  void placeOskButton(float left, float top); // clamped to the root box
  void setOskOpen(bool open);
  void placeOsk(); // docks the keyboard (top / bottom), moves the button off it
  vkpcnx::StreamOverlay::SessionMetrics collectSessionMetrics() const;

  vkpcnx::GameCard::Game game_;
  vkpcnx::stream::PlayUrl playUrl_;
  std::unique_ptr<vkpcnx::stream::StreamSession> session_;
  const bool playgroundMode_ = false;
  std::unique_ptr<vkpcnx::PlaygroundDesktop> playground_;
  bool playgroundStarted_ = false; // first capture done

  bool captured_ = false;
  bool cursorShown_ = true; // server cursor state (§7.6); drawn only while captured
  bool leaving_ = false;
  bool dialogOpen_ = false;
  bool overlayOpen_ = false;
  bool hudVisible_ = false;
  bool oskButtonVisible_ = false;
  bool oskButtonPlaced_ = false;
  float oskButtonLeft_ = 0, oskButtonTop_ = 0; // relative to the root box
  bool oskOpen_ = false;
  bool oskDockTop_ = false;
  uint8_t oskLockKeys_ = 0; // Caps as toggled on the on-screen keyboard
  brls::Event<brls::KeyState>::Subscription keySub_{};
  brls::Event<brls::Point>::Subscription mouseSub_{};
  brls::Event<brls::Point>::Subscription scrollSub_{};
  brls::Event<bool>::Subscription focusSub_{};
  bool haveKeySub_ = false;

  // Per-frame input state
  brls::RawMouseState lastMouse_{};
  float wheelAccum_ = 0; // fractional wheel offsets (trackpads, smooth wheels)
  bool padConnected_[4] = {false, false, false, false};
  bool padButtons_[4][16] = {};
  uint32_t padPov_[4] = {0, 0, 0, 0};
  // brls buttons consumed by a hotkey: not forwarded until physically released
  bool padSuppressed_[4][brls::_BUTTON_MAX] = {};
  bool lastPointerMode_ = false;
  float escHoldSeconds_ = 0;
  bool lctrlDown_ = false, laltDown_ = false, escDown_ = false;
  // Mouse released with Ctrl+Alt: a click in the window captures it again
  bool releasedByUser_ = false;
  float cursorX_ = 0, cursorY_ = 0;
  bool cursorInit_ = false;
  float cursorRemX_ = 0, cursorRemY_ = 0; // sub-pixel carry for analog moves
  std::chrono::steady_clock::time_point lastPollAt_{};

  // Gamepad-as-mouse (/controls/gamepad_pointer)
  bool pointerButtons_[3] = {false, false, false}; // LEFT, RIGHT, MIDDLE
  bool pointerKeys_[7] = {};                       // see kPointerKeys
  bool pointerOskPrev_ = false;

  // On-screen keyboard, gamepad side
  bool oskPadPrev_[brls::_BUTTON_MAX] = {};
  bool oskShoulderChord_ = false; // L and R held together: not a language / Fn tap
  int oskFocusDx_ = 0, oskFocusDy_ = 0;
  float oskFocusRepeat_ = 0;
  // Fingers on the keyboard (typing) vs elsewhere (mouse), by finger id
  std::vector<int> oskFingers_, otherFingers_;
  float pointerScrollAccum_ = 0;
  float pointerHoldSeconds_ = 0; // stick at full deflection, drives acceleration

  // Touchscreen gesture state (/controls/touchscreen_mode)
  struct TouchGesture {
    bool active = false;
    int maxFingers = 0;
    bool moved = false;     // primary finger left the tap slop
    bool leftHeld = false;  // dragging with the left button down
    bool tapDrag = false;   // trackpad: began within the tap-drag window of a tap
    bool rightSent = false; // long-press right click already emitted
    bool onOskButton = false; // started on the on-screen keyboard button: drag it
    brls::Point oskGrab;      // finger offset from the button's origin
    brls::Point start;      // primary finger, window points
    brls::Point last;
    brls::Point lastCentroid;
    float scrollAccum = 0;
    float speed = 0; // smoothed trackpad finger speed, points/s
    std::chrono::steady_clock::time_point downAt{};
  } touch_;
  // Trackpad tap: the left button stays down briefly so a quick second touch
  // can turn it into a drag (tap-and-drag)
  bool tapClickPending_ = false;
  std::chrono::steady_clock::time_point tapClickReleaseAt_{};

  size_t hudTimer_ = 0;
  std::string lastClipboard_;

  // Last (soonest-expiring) value from any SC_*_TIME_LEFT source; the merge
  // across sources happens in GameServerClient itself.
  int64_t lastTimeLeftSeconds_ = 0;

  // Set once, the first time the session reaches State::Streaming; not reset
  // on a mid-session Reconnecting → Streaming round trip.
  std::optional<std::chrono::steady_clock::time_point> sessionStartedAt_;

  std::string gameServerName_; // SC_VM_NAME
};
