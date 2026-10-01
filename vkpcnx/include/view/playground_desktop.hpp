#pragma once

#include <borealis.hpp>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "core/stream/input_sink.hpp"

namespace vkpcnx {

// The controls playground's stand-in for a cloud session: a monochrome
// Windows-like desktop (High Contrast colours, Windows 10 metrics at 100 %
// scale) fed with exactly the input StreamActivity would send to the server.
//
// Sizes are Windows DIPs: desktop pixels of a desktop as tall as the stream a
// real session would request, times the display scale Windows would pick for
// the client's own UI scale (see snapUiScale()). Scaled to the screen like the
// video is, so buttons, check boxes and scroll bars come out as large as they
// would be on the VM.
//
// Windows (draggable by the title bar, with minimize / maximize / close):
//  - a notepad for the keyboard (US / ЙЦУКЕН, Shift, Caps, held keys)
//  - a list box for the wheel, with a draggable scroll bar, plus a push
//    button and a check box to click
//  - the controls settings (mapping, touchscreen mode, gamepad pointer,
//    sensitivity sliders), applied live, above a tester for the virtual Xbox
//    pads without the button B is mapped to (B leaves the playground); its
//    "Схема…" button opens a viewer with the gamepad pointer diagram
// Right click anywhere opens a context menu, left-dragging on the empty
// desktop draws a selection rectangle, and Start opens a modal error box.
class PlaygroundDesktop : public stream::InputSink {
public:
  // desktopHeight: the stream height, in pixels. uiScale: the client's UI
  // scale (1 = 100 %), snapped to what Windows offers for that height.
  PlaygroundDesktop(int desktopHeight, float uiScale);
  ~PlaygroundDesktop() override;

  // Windows' display scale steps (25 %), capped like Windows caps them: the
  // desktop stays at least 576 DIPs tall (720p → 125 %, 1080p → 175 %)
  static float snapUiScale(float uiScale, int desktopHeight);

  // The virtual pad button the labelled B of pad `index` would send (it
  // leaves the playground instead), left out of the pad tester
  void setExitPadButton(int index, uint8_t button) {
    if (index >= 0 && index < 4)
      exitPadButton_[index] = button;
  }

  // rect: the video rectangle, which the cursor positions are relative to
  void draw(NVGcontext *vg, const brls::Rect &rect);

  // InputSink
  void setFocus(bool focused) override;
  void setLockKeys(uint8_t lockKeys) override;
  void keyEvent(uint8_t dik, bool pressed, const std::string &locale = {}) override;
  void mouseMove(int dx, int dy) override;
  void mouseMoveTo(int x, int y) override;
  void mouseButton(uint8_t button, bool pressed) override;
  void mouseWheel(int steps) override;
  void gamepadButton(int index, uint8_t button, bool pressed) override;
  void gamepadAxis(int index, uint8_t axis, int32_t value) override;
  void gamepadPov(int index, uint32_t pov) override;
  void releaseAll() override;
  void sendClipboardText(const std::string &) override {}

private:
  // The message box never gets a taskbar button, the scheme viewer only while
  // it is open
  enum WindowKind { WinNotepad, WinList, WinControls, WinScheme, WinMessageBox, WinCount };
  struct Window {
    const char *title = "";
    float x = 0, y = 0, w = 0, h = 0;
    bool open = true;    // false: closed for good (the scheme viewer)
    bool hidden = false; // minimized or closed
    bool maximized = false;
    float restoreX = 0, restoreY = 0, restoreW = 0, restoreH = 0;
  };
  // What a point is over; a press activates on release over the same thing
  enum class Part {
    None,
    Desktop,
    Taskbar,
    TaskbarButton, // index = window
    Start,
    Caption,
    Minimize,
    Maximize,
    Close,
    Content,
    Button, // the list window's push button / the controls window's "Схема…"
    Check,  // the list window's / the gamepad pointer check box
    Radio,  // controls window: index = group * 2 + option
    Slider, // controls window: index = sensitivity
    DialogOk,
    ListItem, // index = item
    ListUp,
    ListDown,
    ListThumb,
    ListTrack,
    PadSlot,  // index = pad
    MenuItem, // index = item
  };
  struct Hit {
    Part part = Part::None;
    int window = -1;
    int index = 0;
    bool operator==(const Hit &o) const {
      return part == o.part && window == o.window && index == o.index;
    }
  };
  struct Pad {
    bool seen = false;
    uint16_t buttons = 0;
    int32_t axes[6] = {};
    uint32_t pov = 0;
  };

  void layoutWindows();
  brls::Rect contentRect(const Window &w) const;
  brls::Rect windowRect(const Window &w) const { return {w.x, w.y, w.w, w.h}; }
  float taskbarTop() const;

  Hit hitTest(float x, float y) const;
  Hit hitContent(int window, float x, float y) const;
  void pointerMoved();
  void leftDown();
  void leftUp();
  void rightUp();
  void activate(const Hit &hit);
  void bringToFront(int window);
  void toggleMaximize(int window);
  void clampWindow(Window &w);
  void hideWindow(int window); // minimize / close: the next one up gets active
  bool messageBoxOpen() const { return !windows_[WinMessageBox].hidden; }
  void openMessageBox();
  void openScheme();
  std::vector<int> taskbarWindows() const; // in taskbar order

  // List window geometry, in desktop coordinates
  brls::Rect listBox() const;
  brls::Rect listButton() const;
  brls::Rect listCheck() const;
  brls::Rect listTrack() const;
  brls::Rect listThumb() const;
  int listVisibleRows() const;
  void scrollList(float rows);

  void typeKey(uint8_t dik);
  std::string keyName(uint8_t dik) const;
  void releaseKeys();

  void drawDesktop(NVGcontext *vg);
  void drawWindow(NVGcontext *vg, int window);
  void drawNotepadWindow(NVGcontext *vg, const brls::Rect &c);
  void drawListWindow(NVGcontext *vg, const brls::Rect &c);
  void drawControlsWindow(NVGcontext *vg, const brls::Rect &c);
  void dragSlider(int index, float x); // thumb follows the cursor, not saved yet
  void drawMessageBox(NVGcontext *vg, const brls::Rect &c);
  void drawSchemeWindow(NVGcontext *vg, const brls::Rect &c);
  void drawTaskbar(NVGcontext *vg);
  void drawContextMenu(NVGcontext *vg);
  void drawPushButton(
    NVGcontext *vg,
    const brls::Rect &r,
    const char *label,
    bool pressed,
    bool hover,
    bool isDefault = false
  );

  // Geometry: desktop pixels ↔ the video rectangle (cursor space)
  const float uiScale_;  // physical desktop pixels per DIP
  const float desktopH_; // DIPs
  float desktopW_ = 0;
  float scale_ = 1; // video-rect points per desktop pixel
  brls::Rect rect_;
  bool laidOut_ = false;
  bool barnaulWallpaper = false;

  std::vector<Window> windows_;
  std::vector<int> zOrder_; // back to front
  int active_ = -1;         // window with the highlighted title bar

  // Mouse
  float mouseX_ = 0, mouseY_ = 0; // video-rect points
  bool mouseInit_ = false;
  bool buttons_[3] = {false, false, false};
  Hit pressed_; // left button went down here
  Hit hover_;
  int dragWindow_ = -1;
  float dragGrabX_ = 0, dragGrabY_ = 0;
  float thumbGrab_ = 0; // list thumb drag: cursor offset from the thumb top
  bool selecting_ = false;
  float selectX_ = 0, selectY_ = 0;
  std::chrono::steady_clock::time_point lastClickAt_{};
  float lastClickX_ = -100, lastClickY_ = -100;
  Hit lastClickHit_;
  bool doubleClick_ = false; // the current left press is the second of a double click

  // Context menu (right click)
  bool menuOpen_ = false;
  float menuX_ = 0, menuY_ = 0;

  // List window
  static constexpr int kListItems = 100;
  float listScroll_ = 0; // rows
  int listSelected_ = -1;
  bool listChecked_ = false;

  // Controls window: the sensitivity slider being dragged
  int sliderDrag_ = -1;
  float sliderGrab_ = 0; // cursor offset from the thumb centre

  // Scheme viewer: res/img/gamepad_pointer.png, loaded on first draw
  int schemeImage_ = 0;
  bool schemeLoadFailed_ = false;

  // Desktop wallpaper: res/img/playground_wallpaper.jpg, loaded on first draw
  int wallpaperImage_ = 0;
  bool wallpaperLoadFailed_ = false;

  // Message box: a click outside it flashes its title bar, like Windows does
  std::chrono::steady_clock::time_point flashUntil_{};

  // Keyboard
  std::string text_ = "Это — песочница. Здесь можно проверить работу элементов управления, как в "
                      "реальной игровой сессии.";
  std::vector<uint8_t> heldKeys_; // in press order
  bool russian_ = false;
  bool caps_ = false;
  uint8_t repeatDik_ = 0; // Windows repeats the last key pressed
  std::chrono::steady_clock::time_point repeatAt_{};

  // Gamepads
  Pad pads_[4];
  int shownPad_ = 0;
  static constexpr uint8_t kNoButton = 0xFF;
  uint8_t exitPadButton_[4] = {kNoButton, kNoButton, kNoButton, kNoButton};
};

} // namespace vkpcnx
