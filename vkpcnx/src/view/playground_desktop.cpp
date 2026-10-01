#include "view/playground_desktop.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "core/controls.hpp"
#include "core/settings.hpp"
#include "core/stream/input_encoder.hpp"

namespace vkpcnx {

using namespace vkpcnx::stream;
using clock_type = std::chrono::steady_clock;

namespace {

// Windows 10 metrics at 100 % scale, desktop pixels
constexpr float kTitleH = 30;   // caption bar
constexpr float kCaptionW = 46; // minimize / maximize / close
constexpr float kTaskbarH = 40;
constexpr float kStartW = 48;
constexpr float kTaskButtonW = 160;
constexpr float kButtonW = 75, kButtonH = 23; // dialog push button
constexpr float kCheck = 13;                  // check box / radio button
constexpr float kScrollW = 17;                // scroll bar width, arrow size
constexpr float kRowH = 20;                   // list box item
constexpr float kMenuItemH = 22, kMenuW = 200;
constexpr float kFont = 12; // Segoe UI 9 pt
constexpr float kNotepadFont = 15, kNotepadLineH = 19;
constexpr float kDoubleClickSlop = 4; // SM_CXDOUBLECLK / 2
constexpr auto kDoubleClickTime = std::chrono::milliseconds(500);
constexpr auto kRepeatDelay = std::chrono::milliseconds(500);
constexpr auto kRepeatInterval = std::chrono::milliseconds(33);
constexpr int kWheelLines = 3; // SPI_GETWHEELSCROLLLINES default

// Message box: Windows 10 layout (MessageBox with MB_OK | MB_ICONERROR)
constexpr float kMessageBoxW = 400, kMessageBoxH = 170;
constexpr float kMessageBoxFooterH = 42, kMessageBoxIcon = 32;
const char *const kMessageBoxTitle = "Ошибка Windows";
const char *const kMessageBoxText =
  "У вас ненастоящий Windows. Обратитесь к системному администратору.";
constexpr auto kFlashTime = std::chrono::milliseconds(600);

// Controls window: settings on top, the pad tester below kPadTop. Defaults
// are the ones StreamActivity uses.
constexpr float kPadTop = 262;
constexpr float kSchemeImageW = 650, kSchemeImageH = 360;   // half the PNG
constexpr float kTrackbarThumbW = 11, kTrackbarThumbH = 21; // Windows 10 trackbar
struct RadioGroup {
  const char *label, *key, *def;
  const char *values[2], *names[2];
};
const RadioGroup kRadioGroups[] = {
  {"Раскладка контроллера:",
   "/controls/mapping_mode",
   "positional",
   {"literal", "positional"},
   {"Буквальная", "Позиционная"}},
  {"Сенсорный экран:",
   "/controls/touchscreen_mode",
   "touchscreen",
   {"trackpad", "touchscreen"},
   {"Трекпад", "Тачскрин"}},
};
const struct {
  const char *label;
  const controls::Sensitivity *sensitivity;
} kSliders[] = {
  {"Скорость указателя (правый стик)", &controls::kGamepadCursor},
  {"Скорость прокрутки (левый стик)", &controls::kGamepadScroll},
  {"Скорость указателя (трекпад)", &controls::kTrackpad},
};
constexpr int kSliderCount = sizeof kSliders / sizeof kSliders[0];

const char *const kMenuItems[] = {"Обновить", "Вид", "Персонализация", "Свойства"};
constexpr int kMenuCount = sizeof kMenuItems / sizeof kMenuItems[0];

NVGcolor gray(int v, int a = 255) { return nvgRGBA(v, v, v, a); }
const NVGcolor kBlack = gray(0), kWhite = gray(255), kHint = gray(150);

bool inside(const brls::Rect &r, float x, float y) {
  return x >= r.getMinX() && x < r.getMaxX() && y >= r.getMinY() && y < r.getMaxY();
}

// r moved from content-relative to desktop coordinates
brls::Rect at(const brls::Rect &c, float x, float y, float w, float h) {
  return {c.getMinX() + x, c.getMinY() + y, w, h};
}

void fill(NVGcontext *vg, const brls::Rect &r, NVGcolor color) {
  nvgBeginPath(vg);
  nvgRect(vg, r.getMinX(), r.getMinY(), r.getWidth(), r.getHeight());
  nvgFillColor(vg, color);
  nvgFill(vg);
}

// 1 px border inside r
void stroke(NVGcontext *vg, const brls::Rect &r, NVGcolor color, float width = 1) {
  nvgBeginPath(vg);
  nvgRect(
    vg,
    r.getMinX() + width / 2,
    r.getMinY() + width / 2,
    r.getWidth() - width,
    r.getHeight() - width
  );
  nvgStrokeColor(vg, color);
  nvgStrokeWidth(vg, width);
  nvgStroke(vg);
}

void line(NVGcontext *vg, float x0, float y0, float x1, float y1, NVGcolor color, float width = 1) {
  nvgBeginPath(vg);
  nvgMoveTo(vg, x0, y0);
  nvgLineTo(vg, x1, y1);
  nvgStrokeColor(vg, color);
  nvgStrokeWidth(vg, width);
  nvgStroke(vg);
}

// Returns the advance
float text(
  NVGcontext *vg,
  float x,
  float y,
  const std::string &s,
  NVGcolor color,
  int align = NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE,
  float size = kFont
) {
  nvgFontFaceId(vg, brls::Application::getDefaultFont());
  nvgFontSize(vg, size);
  nvgTextAlign(vg, align);
  nvgFillColor(vg, color);
  return nvgText(vg, x, y, s.c_str(), nullptr) - x;
}

float textWidth(NVGcontext *vg, const std::string &s, float size = kFont) {
  nvgFontFaceId(vg, brls::Application::getDefaultFont());
  nvgFontSize(vg, size);
  return nvgTextBounds(vg, 0, 0, s.c_str(), nullptr, nullptr);
}

// UTF-8 string → one string per code point
std::vector<std::string> splitChars(const char *s) {
  std::vector<std::string> out;
  while (*s) {
    unsigned char c = static_cast<unsigned char>(*s);
    int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
    out.emplace_back(s, len);
    s += len;
  }
  return out;
}

// Character keys on the US and ЙЦУКЕН layouts, runs of consecutive DIK codes
// (§9.1): plain / shifted for each layout
struct KeyRow {
  uint8_t firstDik;
  const char *en, *enShift, *ru, *ruShift;
};
const KeyRow kKeyRows[] = {
  {2, "1234567890-=", "!@#$%^&*()_+", "1234567890-=", "!\"№;%:?*()_+"},
  {16, "qwertyuiop[]", "QWERTYUIOP{}", "йцукенгшщзхъ", "ЙЦУКЕНГШЩЗХЪ"},
  {30, "asdfghjkl;'`", "ASDFGHJKL:\"~", "фывапролджэё", "ФЫВАПРОЛДЖЭЁ"},
  {43, "\\zxcvbnm,./", "|ZXCVBNM<>?", "\\ячсмитьбю.", "/ЯЧСМИТЬБЮ,"},
};

struct KeyChars {
  std::string plain, shifted;
  bool letter = false; // Caps Lock applies
};

// false: not a character key
bool keyChars(uint8_t dik, bool russian, KeyChars &out) {
  for (const auto &row : kKeyRows) {
    auto plain = splitChars(russian ? row.ru : row.en);
    if (dik < row.firstDik || dik >= row.firstDik + plain.size())
      continue;
    size_t i = dik - row.firstDik;
    out.plain = plain[i];
    out.shifted = splitChars(russian ? row.ruShift : row.enShift)[i];
    out.letter = out.plain.size() > 1 || std::isalpha(static_cast<unsigned char>(out.plain[0]));
    return true;
  }
  static const char kNumpad[] = "789-456+1230."; // DIK 71..83
  if (dik >= 71 && dik <= 83) {
    out.plain = out.shifted = std::string(1, kNumpad[dik - 71]);
    return true;
  }
  if (dik == 55 || dik == 181) {
    out.plain = out.shifted = dik == 55 ? "*" : "/";
    return true;
  }
  return false;
}

bool isModifier(uint8_t dik) {
  switch (dik) {
  case 29: // Ctrl
  case 157:
  case 42: // Shift
  case 54:
  case 56: // Alt
  case 184:
  case 219: // Win
  case 220:
  case 58: // Caps Lock
  case 69: // Num Lock
  case 70: // Scroll Lock
    return true;
  default:
    return false;
  }
}

} // namespace

float PlaygroundDesktop::snapUiScale(float uiScale, int desktopHeight) {
  constexpr float kStep = 0.25f, kMinHeight = 480;
  float maxScale = std::floor(desktopHeight / kMinHeight / kStep) * kStep;
  float snapped = std::round(uiScale / kStep) * kStep;
  return std::max(1.0f, std::min(snapped, maxScale));
}

PlaygroundDesktop::PlaygroundDesktop(int desktopHeight, float uiScale)
    : uiScale_(snapUiScale(uiScale, std::max(desktopHeight, 480))),
      desktopH_(std::max(desktopHeight, 480) / uiScale_) {}

PlaygroundDesktop::~PlaygroundDesktop() {
  if (schemeImage_)
    nvgDeleteImage(brls::Application::getNVGContext(), schemeImage_);
  if (wallpaperImage_)
    nvgDeleteImage(brls::Application::getNVGContext(), wallpaperImage_);
}

float PlaygroundDesktop::taskbarTop() const { return desktopH_ - kTaskbarH; }

void PlaygroundDesktop::layoutWindows() {
  laidOut_ = true;
  windows_.assign(WinCount, Window{});
  auto place = [&](WindowKind k, const char *title, float x, float y, float w, float h) {
    auto &win = windows_[k];
    win.title = title;
    win.x = x;
    win.y = y;
    win.w = w;
    win.h = h;
    clampWindow(win);
  };
  place(WinNotepad, "Блокнот", 40, 40, 440, 300);
  place(WinList, "Список", 510, 40, 240, 362);
  place(WinControls, "Настройки управления", 780, 40, 380, 532);
  place(
    WinMessageBox,
    kMessageBoxTitle,
    (desktopW_ - kMessageBoxW) / 2,
    (taskbarTop() - kMessageBoxH) / 2,
    kMessageBoxW,
    kMessageBoxH
  );
  place(WinScheme, "Контроллер как мышь", 0, 0, kSchemeImageW + 2, kSchemeImageH + 2 + kTitleH);
  windows_[WinScheme].open = false;
  windows_[WinScheme].hidden = true;
  windows_[WinMessageBox].hidden = true;
  zOrder_ = {WinMessageBox, WinScheme, WinControls, WinList, WinNotepad};
  active_ = WinNotepad;
}

void PlaygroundDesktop::clampWindow(Window &w) {
  if (w.maximized) {
    w.x = w.y = 0;
    w.w = desktopW_;
    w.h = taskbarTop();
    return;
  }
  // Like Windows: the title bar stays reachable
  w.x = std::clamp(w.x, 80 - w.w, std::max(0.0f, desktopW_ - 80));
  w.y = std::clamp(w.y, 0.0f, std::max(0.0f, taskbarTop() - kTitleH));
}

brls::Rect PlaygroundDesktop::contentRect(const Window &w) const {
  return {w.x + 1, w.y + 1 + kTitleH, w.w - 2, w.h - 2 - kTitleH};
}

// ---- window content geometry -------------------------------------------------

static brls::Rect padSlotRect(const brls::Rect &c, int i) {
  return at(c, 16 + i * 30, kPadTop + 10, 24, 20);
}
// Radio buttons and the check box: the label is part of the hit area
static brls::Rect radioRect(const brls::Rect &c, int group, int option) {
  return at(c, 16 + option * 150, 30 + group * 44, 140, 16);
}
static brls::Rect pointerCheckRect(const brls::Rect &c) { return at(c, 16, 102, 200, 16); }
// "Схема управления", on the check box's row
static brls::Rect schemeButtonRect(const brls::Rect &c) {
  return at(c, c.getWidth() - 16 - (kButtonW + 75), 110 - kButtonH / 2, (kButtonW + 75), kButtonH);
}
// A slider's label row sits right above this; the channel runs along its middle
static brls::Rect sliderRect(const brls::Rect &c, int i) {
  return at(c, 16, 146 + i * 38, c.getWidth() - 32, 24);
}
static float sliderX(const brls::Rect &r, float progress) { // thumb centre
  return r.getMinX() + kTrackbarThumbW / 2 + progress * (r.getWidth() - kTrackbarThumbW);
}

// ---- list window geometry ------------------------------------------------------

// The push button and the check box share a row under the list box
brls::Rect PlaygroundDesktop::listBox() const {
  auto c = contentRect(windows_[WinList]);
  return at(c, 8, 8, c.getWidth() - 16, c.getHeight() - 24 - kButtonH);
}

brls::Rect PlaygroundDesktop::listButton() const {
  auto c = contentRect(windows_[WinList]);
  return at(c, c.getWidth() - 8 - kButtonW, c.getHeight() - 8 - kButtonH, kButtonW, kButtonH);
}

// The label is part of the hit area, like on Windows
brls::Rect PlaygroundDesktop::listCheck() const {
  auto c = contentRect(windows_[WinList]);
  return at(c, 8, c.getHeight() - 8 - kButtonH / 2 - 8, 110, 16);
}

static brls::Rect messageBoxOk(const brls::Rect &c) {
  return at(
    c,
    c.getWidth() - 11 - kButtonW,
    c.getHeight() - (kMessageBoxFooterH + kButtonH) / 2,
    kButtonW,
    kButtonH
  );
}

brls::Rect PlaygroundDesktop::listTrack() const {
  auto b = listBox();
  return {
    b.getMaxX() - 1 - kScrollW,
    b.getMinY() + 1 + kScrollW,
    kScrollW,
    std::max(0.0f, b.getHeight() - 2 - 2 * kScrollW)
  };
}

int PlaygroundDesktop::listVisibleRows() const {
  return std::max(1, static_cast<int>((listBox().getHeight() - 2) / kRowH));
}

brls::Rect PlaygroundDesktop::listThumb() const {
  auto track = listTrack();
  float maxScroll = std::max(0, kListItems - listVisibleRows());
  float h = std::max(kScrollW, track.getHeight() * listVisibleRows() / kListItems);
  h = std::min(h, track.getHeight());
  float t = maxScroll > 0 ? listScroll_ / maxScroll : 0;
  return {track.getMinX(), track.getMinY() + (track.getHeight() - h) * t, kScrollW, h};
}

void PlaygroundDesktop::scrollList(float rows) {
  float maxScroll = std::max(0, kListItems - listVisibleRows());
  listScroll_ = std::clamp(listScroll_ + rows, 0.0f, maxScroll);
}

// ---- hit testing -----------------------------------------------------------------

PlaygroundDesktop::Hit PlaygroundDesktop::hitTest(float x, float y) const {
  if (menuOpen_) {
    brls::Rect menu(menuX_, menuY_, kMenuW, 4 + kMenuCount * kMenuItemH);
    if (inside(menu, x, y)) {
      int i = static_cast<int>((y - menuY_ - 2) / kMenuItemH);
      return {Part::MenuItem, -1, std::clamp(i, 0, kMenuCount - 1)};
    }
  }
  if (y >= taskbarTop()) {
    if (x < kStartW)
      return {Part::Start, -1, 0};
    float bx = x - kStartW;
    auto ids = taskbarWindows();
    if (bx >= 0 && bx < ids.size() * kTaskButtonW)
      return {Part::TaskbarButton, ids[static_cast<size_t>(bx / kTaskButtonW)], 0};
    return {Part::Taskbar, -1, 0};
  }
  for (auto it = zOrder_.rbegin(); it != zOrder_.rend(); ++it) {
    const auto &w = windows_[*it];
    if (w.hidden || !inside(windowRect(w), x, y))
      continue;
    if (y < w.y + 1 + kTitleH) {
      float fromRight = w.x + w.w - 1 - x;
      if (fromRight >= 0 && fromRight < kCaptionW)
        return {Part::Close, *it, 0};
      if (*it == WinMessageBox) // close only
        return {Part::Caption, *it, 0};
      if (fromRight >= kCaptionW && fromRight < 2 * kCaptionW)
        return {Part::Maximize, *it, 0};
      if (fromRight >= 2 * kCaptionW && fromRight < 3 * kCaptionW)
        return {Part::Minimize, *it, 0};
      return {Part::Caption, *it, 0};
    }
    return hitContent(*it, x, y);
  }
  return {Part::Desktop, -1, 0};
}

PlaygroundDesktop::Hit PlaygroundDesktop::hitContent(int window, float x, float y) const {
  auto c = contentRect(windows_[window]);
  switch (window) {
  case WinMessageBox:
    if (inside(messageBoxOk(c), x, y))
      return {Part::DialogOk, window, 0};
    break;
  case WinList: {
    if (inside(listButton(), x, y))
      return {Part::Button, window, 0};
    if (inside(listCheck(), x, y))
      return {Part::Check, window, 0};
    auto box = listBox();
    if (!inside(box, x, y))
      break;
    if (x >= box.getMaxX() - 1 - kScrollW) {
      if (y < box.getMinY() + 1 + kScrollW)
        return {Part::ListUp, window, 0};
      if (y >= box.getMaxY() - 1 - kScrollW)
        return {Part::ListDown, window, 0};
      if (inside(listThumb(), x, y))
        return {Part::ListThumb, window, 0};
      return {Part::ListTrack, window, 0};
    }
    int item = static_cast<int>(std::floor(listScroll_ + (y - box.getMinY() - 1) / kRowH));
    if (item >= 0 && item < kListItems)
      return {Part::ListItem, window, item};
    break;
  }
  case WinControls:
    for (int g = 0; g < 2; g++)
      for (int o = 0; o < 2; o++)
        if (inside(radioRect(c, g, o), x, y))
          return {Part::Radio, window, g * 2 + o};
    if (inside(pointerCheckRect(c), x, y))
      return {Part::Check, window, 0};
    if (inside(schemeButtonRect(c), x, y))
      return {Part::Button, window, 0};
    for (int i = 0; i < kSliderCount; i++)
      if (inside(sliderRect(c, i), x, y))
        return {Part::Slider, window, i};
    for (int i = 0; i < 4; i++)
      if (inside(padSlotRect(c, i), x, y))
        return {Part::PadSlot, window, i};
    break;
  default:
    break;
  }
  return {Part::Content, window, 0};
}

// ---- mouse -----------------------------------------------------------------------

void PlaygroundDesktop::mouseMove(int dx, int dy) { mouseMoveTo(mouseX_ + dx, mouseY_ + dy); }

void PlaygroundDesktop::mouseMoveTo(int x, int y) {
  // Same clamping as StreamActivity's cursor
  float maxX = rect_.getWidth() > 0 ? rect_.getWidth() : 1e9f;
  float maxY = rect_.getHeight() > 0 ? rect_.getHeight() : 1e9f;
  mouseX_ = std::clamp(static_cast<float>(x), 0.0f, maxX);
  mouseY_ = std::clamp(static_cast<float>(y), 0.0f, maxY);
  pointerMoved();
}

void PlaygroundDesktop::pointerMoved() {
  if (!laidOut_)
    return;
  float x = mouseX_ / scale_, y = mouseY_ / scale_;
  hover_ = hitTest(x, y);
  if (!buttons_[pk::MOUSE_LEFT])
    return;
  if (dragWindow_ >= 0) {
    auto &w = windows_[dragWindow_];
    w.x = x - dragGrabX_;
    w.y = y - dragGrabY_;
    clampWindow(w);
  } else if (sliderDrag_ >= 0) {
    dragSlider(sliderDrag_, x);
  } else if (pressed_.part == Part::ListThumb) {
    auto track = listTrack();
    auto thumb = listThumb();
    float range = track.getHeight() - thumb.getHeight();
    float maxScroll = std::max(0, kListItems - listVisibleRows());
    if (range > 0)
      listScroll_ =
        std::clamp((y - thumbGrab_ - track.getMinY()) / range * maxScroll, 0.0f, maxScroll);
  }
}

void PlaygroundDesktop::mouseButton(uint8_t button, bool pressed) {
  if (button >= 3 || buttons_[button] == pressed)
    return;
  buttons_[button] = pressed;
  if (!laidOut_)
    return;
  if (button == pk::MOUSE_LEFT)
    pressed ? leftDown() : leftUp();
  else if (button == pk::MOUSE_RIGHT && !pressed)
    rightUp();
}

void PlaygroundDesktop::mouseWheel(int steps) {
  if (!laidOut_ || messageBoxOpen())
    return;
  // Windows 10 scrolls the window under the cursor, active or not
  Hit h = hitTest(mouseX_ / scale_, mouseY_ / scale_);
  if (h.window == WinList && inside(listBox(), mouseX_ / scale_, mouseY_ / scale_))
    scrollList(static_cast<float>(-steps * kWheelLines));
}

void PlaygroundDesktop::leftDown() {
  float x = mouseX_ / scale_, y = mouseY_ / scale_;
  Hit h = hitTest(x, y);

  auto now = clock_type::now();
  doubleClick_ = now - lastClickAt_ <= kDoubleClickTime &&
                 std::fabs(x - lastClickX_) <= kDoubleClickSlop &&
                 std::fabs(y - lastClickY_) <= kDoubleClickSlop && h == lastClickHit_;
  // A third click starts a new pair
  lastClickAt_ = doubleClick_ ? clock_type::time_point{} : now;
  lastClickX_ = x;
  lastClickY_ = y;
  lastClickHit_ = h;

  if (menuOpen_) {
    if (h.part == Part::MenuItem) {
      pressed_ = h;
    } else {
      menuOpen_ = false; // a click outside only closes the menu
      pressed_ = {};
    }
    return;
  }

  // Modal: anything outside the message box only flashes it
  if (messageBoxOpen() && h.window != WinMessageBox) {
    flashUntil_ = now + kFlashTime;
    pressed_ = {};
    return;
  }

  pressed_ = h;
  if (h.window >= 0 && h.part != Part::TaskbarButton) // that one acts on release
    bringToFront(h.window);
  switch (h.part) {
  case Part::Caption: {
    auto &w = windows_[h.window];
    if (doubleClick_) {
      toggleMaximize(h.window);
      pressed_ = {};
    } else if (!w.maximized) {
      dragWindow_ = h.window;
      dragGrabX_ = x - w.x;
      dragGrabY_ = y - w.y;
    }
    break;
  }
  case Part::ListUp:
    scrollList(-1);
    break;
  case Part::ListDown:
    scrollList(1);
    break;
  case Part::ListTrack:
    scrollList(y < listThumb().getMinY() ? -listVisibleRows() : listVisibleRows());
    break;
  case Part::ListThumb:
    thumbGrab_ = y - listThumb().getMinY();
    break;
  case Part::Slider: {
    // On the thumb: drag it. On the channel: a page (1/8) towards the click,
    // then drag from there, like a Windows trackbar
    const auto &sens = *kSliders[h.index].sensitivity;
    auto r = sliderRect(contentRect(windows_[WinControls]), h.index);
    float progress = controls::toProgress(sens, controls::get(sens));
    float thumb = sliderX(r, progress);
    if (std::fabs(x - thumb) > kTrackbarThumbW / 2) {
      progress = std::clamp(progress + (x > thumb ? 0.125f : -0.125f), 0.0f, 1.0f);
      controls::set(sens, controls::fromProgress(sens, progress), false);
      thumb = sliderX(r, controls::toProgress(sens, controls::get(sens)));
    }
    sliderDrag_ = h.index;
    sliderGrab_ = x - thumb;
    break;
  }
  case Part::ListItem:
    listSelected_ = h.index;
    break;
  case Part::Desktop:
    selecting_ = true;
    selectX_ = x;
    selectY_ = y;
    active_ = -1;
    break;
  default:
    break;
  }
}

void PlaygroundDesktop::leftUp() {
  Hit h = hitTest(mouseX_ / scale_, mouseY_ / scale_);
  if (pressed_.part != Part::None && h == pressed_)
    activate(h);
  if (sliderDrag_ >= 0) {
    sliderDrag_ = -1;
    Settings::instance().save(); // the drag only changed the in-memory value
  }
  pressed_ = {};
  dragWindow_ = -1;
  selecting_ = false;
}

void PlaygroundDesktop::rightUp() {
  float x = mouseX_ / scale_, y = mouseY_ / scale_;
  Hit h = hitTest(x, y);
  if (messageBoxOpen()) {
    if (h.window != WinMessageBox)
      flashUntil_ = clock_type::now() + kFlashTime;
    return;
  }
  if (h.window >= 0)
    bringToFront(h.window);
  menuOpen_ = true;
  float menuH = 4 + kMenuCount * kMenuItemH;
  menuX_ = x + kMenuW <= desktopW_ ? x : std::max(0.0f, x - kMenuW);
  menuY_ = y + menuH <= desktopH_ ? y : std::max(0.0f, y - menuH);
}

void PlaygroundDesktop::activate(const Hit &h) {
  switch (h.part) {
  case Part::Close:
    if (h.window == WinScheme)
      windows_[WinScheme].open = false; // off the taskbar too
    hideWindow(h.window);
    break;
  case Part::Minimize:
  case Part::DialogOk:
    hideWindow(h.window);
    break;
  case Part::Button:
    if (h.window == WinControls)
      openScheme();
    if (h.window == WinList) {
      barnaulWallpaper = !barnaulWallpaper;
      wallpaperImage_ = 0;
    }
    break;
  case Part::Start:
    openMessageBox();
    break;
  case Part::Maximize:
    toggleMaximize(h.window);
    break;
  case Part::TaskbarButton: {
    auto &w = windows_[h.window];
    if (w.hidden) {
      w.hidden = false;
      bringToFront(h.window);
    } else if (active_ == h.window) {
      activate({Part::Minimize, h.window, 0});
    } else {
      bringToFront(h.window);
    }
    break;
  }
  case Part::Check:
    if (h.window == WinList) {
      listChecked_ = !listChecked_;
    } else { // StreamActivity picks the change up on its next frame
      auto &settings = Settings::instance();
      settings.set(
        "/controls/gamepad_pointer", !settings.get<bool>("/controls/gamepad_pointer", false)
      );
    }
    break;
  case Part::Radio: {
    const auto &g = kRadioGroups[h.index / 2];
    Settings::instance().set(g.key, std::string(g.values[h.index % 2]));
    break;
  }
  case Part::MenuItem:
    menuOpen_ = false;
    break;
  case Part::PadSlot:
    shownPad_ = h.index;
    break;
  default:
    break;
  }
}

void PlaygroundDesktop::dragSlider(int index, float x) {
  const auto &sens = *kSliders[index].sensitivity;
  auto r = sliderRect(contentRect(windows_[WinControls]), index);
  float progress = (x - sliderGrab_ - r.getMinX() - kTrackbarThumbW / 2) /
                   std::max(1.0f, r.getWidth() - kTrackbarThumbW);
  controls::set(sens, controls::fromProgress(sens, progress), false);
}

void PlaygroundDesktop::hideWindow(int window) {
  windows_[window].hidden = true;
  active_ = -1;
  for (auto it = zOrder_.rbegin(); it != zOrder_.rend(); ++it)
    if (!windows_[*it].hidden) {
      active_ = *it;
      break;
    }
}

void PlaygroundDesktop::openMessageBox() {
  auto &w = windows_[WinMessageBox];
  if (w.hidden) { // centred again each time, like a fresh MessageBox()
    w.x = (desktopW_ - w.w) / 2;
    w.y = (taskbarTop() - w.h) / 2;
    clampWindow(w);
  }
  w.hidden = false;
  menuOpen_ = false;
  repeatDik_ = 0; // a held key stops typing into the notepad behind it
  bringToFront(WinMessageBox);
}

void PlaygroundDesktop::openScheme() {
  auto &w = windows_[WinScheme];
  if (!w.open) { // a fresh window opens centred
    w.open = true;
    w.maximized = false;
    w.w = kSchemeImageW + 2;
    w.h = kSchemeImageH + 2 + kTitleH;
    w.x = (desktopW_ - w.w) / 2;
    w.y = (taskbarTop() - w.h) / 2;
    clampWindow(w);
  }
  w.hidden = false;
  bringToFront(WinScheme);
}

std::vector<int> PlaygroundDesktop::taskbarWindows() const {
  std::vector<int> ids;
  for (int i = 0; i < WinCount; i++)
    if (i != WinMessageBox && windows_[i].open)
      ids.push_back(i);
  return ids;
}

void PlaygroundDesktop::bringToFront(int window) {
  zOrder_.erase(std::remove(zOrder_.begin(), zOrder_.end(), window), zOrder_.end());
  zOrder_.push_back(window);
  active_ = window;
}

void PlaygroundDesktop::toggleMaximize(int window) {
  auto &w = windows_[window];
  if (w.maximized) {
    w.maximized = false;
    w.x = w.restoreX;
    w.y = w.restoreY;
    w.w = w.restoreW;
    w.h = w.restoreH;
  } else {
    w.restoreX = w.x;
    w.restoreY = w.y;
    w.restoreW = w.w;
    w.restoreH = w.h;
    w.maximized = true;
  }
  clampWindow(w);
  scrollList(0); // the list may have grown
}

// ---- keyboard ----------------------------------------------------------------------

void PlaygroundDesktop::setLockKeys(uint8_t lockKeys) { caps_ = lockKeys & pk::LOCK_CAPS; }

void PlaygroundDesktop::keyEvent(uint8_t dik, bool pressed, const std::string &locale) {
  if (!locale.empty())
    russian_ = locale == "ru-RU";
  auto held = std::find(heldKeys_.begin(), heldKeys_.end(), dik);
  if (!pressed) {
    if (held != heldKeys_.end())
      heldKeys_.erase(held);
    if (dik == repeatDik_)
      repeatDik_ = 0;
    return;
  }
  if (held != heldKeys_.end())
    return;
  heldKeys_.push_back(dik);
  if (messageBoxOpen()) {
    // Modal: Enter, Space and Esc dismiss it, nothing gets typed
    if (dik == 28 || dik == 156 || dik == 57 || dik == 1)
      hideWindow(WinMessageBox);
    return;
  }
  if (isModifier(dik))
    return;
  typeKey(dik);
  repeatDik_ = dik;
  repeatAt_ = clock_type::now() + kRepeatDelay;
}

void PlaygroundDesktop::typeKey(uint8_t dik) {
  auto isHeld = [&](uint8_t k) {
    return std::find(heldKeys_.begin(), heldKeys_.end(), k) != heldKeys_.end();
  };
  if (isHeld(29) || isHeld(157) || isHeld(56) || isHeld(184))
    return; // shortcuts, not text
  constexpr size_t kMaxText = 4000;
  switch (dik) {
  case 14: // Backspace: one code point
    while (!text_.empty() && (static_cast<unsigned char>(text_.back()) & 0xC0) == 0x80)
      text_.pop_back();
    if (!text_.empty())
      text_.pop_back();
    return;
  case 28:
  case 156:
    text_ += '\n';
    return;
  case 15:
    text_ += "    ";
    return;
  case 57:
    text_ += ' ';
    break;
  default: {
    KeyChars k;
    if (!keyChars(dik, russian_, k))
      return;
    bool shift = isHeld(42) || isHeld(54);
    text_ += (shift != (k.letter && caps_)) ? k.shifted : k.plain;
  }
  }
  if (text_.size() > kMaxText)
    text_.erase(0, text_.size() - kMaxText);
}

std::string PlaygroundDesktop::keyName(uint8_t dik) const {
  switch (dik) {
  case 1:
    return "Esc";
  case 14:
    return "Backspace";
  case 15:
    return "Tab";
  case 28:
    return "Enter";
  case 29:
  case 157:
    return "Ctrl";
  case 42:
  case 54:
    return "Shift";
  case 56:
  case 184:
    return "Alt";
  case 57:
    return "Space";
  case 58:
    return "Caps Lock";
  case 69:
    return "Num Lock";
  case 70:
    return "Scroll Lock";
  case 87:
    return "F11";
  case 88:
    return "F12";
  case 156:
    return "Num Enter";
  case 197:
    return "Pause";
  case 199:
    return "Home";
  case 200:
    return "Up";
  case 201:
    return "PgUp";
  case 203:
    return "Left";
  case 205:
    return "Right";
  case 207:
    return "End";
  case 208:
    return "Down";
  case 209:
    return "PgDn";
  case 210:
    return "Ins";
  case 211:
    return "Del";
  case 219:
  case 220:
    return "Win";
  case 221:
    return "Menu";
  default:
    break;
  }
  if (dik >= 59 && dik <= 68)
    return "F" + std::to_string(dik - 58);
  KeyChars k;
  if (keyChars(dik, false, k)) {
    std::string s = k.plain;
    if (k.letter)
      s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return (dik >= 71 && dik <= 83) || dik == 55 || dik == 181 ? "Num " + s : s;
  }
  return "#" + std::to_string(dik);
}

void PlaygroundDesktop::releaseKeys() {
  heldKeys_.clear();
  repeatDik_ = 0;
}

// ---- gamepads, focus -----------------------------------------------------------------

void PlaygroundDesktop::gamepadButton(int index, uint8_t button, bool pressed) {
  if (index < 0 || index >= 4 || button >= 16)
    return;
  auto &pad = pads_[index];
  pad.seen = true;
  if (pressed) {
    pad.buttons |= 1u << button;
    shownPad_ = index;
  } else {
    pad.buttons &= ~(1u << button);
  }
}

void PlaygroundDesktop::gamepadAxis(int index, uint8_t axis, int32_t value) {
  if (index < 0 || index >= 4 || axis >= 6)
    return;
  pads_[index].seen = true;
  pads_[index].axes[axis] = value;
  if (std::abs(value) > 16000)
    shownPad_ = index;
}

void PlaygroundDesktop::gamepadPov(int index, uint32_t pov) {
  if (index < 0 || index >= 4)
    return;
  pads_[index].seen = true;
  pads_[index].pov = pov;
  if (pov)
    shownPad_ = index;
}

void PlaygroundDesktop::releaseAll() {
  // §8.6: buttons, keys and POV; the axes keep their values
  for (auto &pad : pads_) {
    pad.buttons = 0;
    pad.pov = 0;
  }
  releaseKeys();
  for (int b = 0; b < 3; b++)
    buttons_[b] = false;
  pressed_ = {};
  dragWindow_ = -1;
  selecting_ = false;
  if (sliderDrag_ >= 0) {
    sliderDrag_ = -1;
    Settings::instance().save();
  }
}

void PlaygroundDesktop::setFocus(bool focused) {
  if (!focused)
    releaseAll();
}

// ---- drawing -----------------------------------------------------------------------

void PlaygroundDesktop::draw(NVGcontext *vg, const brls::Rect &rect) {
  if (rect.getWidth() <= 0 || rect.getHeight() <= 0)
    return;
  rect_ = rect;
  scale_ = rect.getHeight() / desktopH_;
  desktopW_ = rect.getWidth() / scale_;
  if (!laidOut_)
    layoutWindows();
  for (auto &w : windows_)
    clampWindow(w); // the window may have been resized
  scrollList(0);
  if (!mouseInit_) {
    // StreamActivity's first capture puts the cursor in the middle too
    mouseInit_ = true;
    mouseX_ = rect.getWidth() / 2;
    mouseY_ = rect.getHeight() / 2;
    pointerMoved();
  }

  auto now = clock_type::now();
  if (repeatDik_ && now >= repeatAt_) {
    typeKey(repeatDik_);
    repeatAt_ = std::max(repeatAt_ + kRepeatInterval, now);
  }

  nvgSave(vg);
  nvgScissor(vg, rect.getMinX(), rect.getMinY(), rect.getWidth(), rect.getHeight());
  nvgTranslate(vg, rect.getMinX(), rect.getMinY());
  nvgScale(vg, scale_, scale_);
  drawDesktop(vg);
  for (int w : zOrder_)
    if (!windows_[w].hidden)
      drawWindow(vg, w);
  drawTaskbar(vg);
  if (menuOpen_)
    drawContextMenu(vg);
  nvgRestore(vg);
}

void PlaygroundDesktop::drawDesktop(NVGcontext *vg) {
  fill(vg, {0, 0, desktopW_, desktopH_}, kBlack);

  auto wallpaperResource =
    barnaulWallpaper ? "img/playground_wallpaper2.jpg" : "img/playground_wallpaper.png";

  if (!wallpaperImage_ && !wallpaperLoadFailed_) {
    wallpaperImage_ =
      nvgCreateImage(vg, (std::string(BRLS_RESOURCES) + wallpaperResource).c_str(), 0);
    wallpaperLoadFailed_ = !wallpaperImage_;
  }
  // Default wallpaper is centered, barnaul wallpaper is stretched (default)
  float ox, oy;
  float scaledW = desktopW_;
  float scaledH = desktopH_;
  if (!barnaulWallpaper && wallpaperImage_) {
    int imgW, imgH;
    nvgImageSize(vg, wallpaperImage_, &imgW, &imgH);
    float scaleX = desktopW_ / (float)imgW;
    float scaleY = desktopH_ / (float)imgH;
    float scale = (scaleX > scaleY) ? scaleX : scaleY;

    if (scale < 1.0f)
      scale = 1.0f;

    scaledW = (float)imgW * scale;
    scaledH = (float)imgH * scale;
    ox = (desktopW_ - scaledW) * 0.5f;
    oy = (desktopH_ - scaledH) * 0.5f;
  }
  if (wallpaperImage_) {
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, desktopW_, desktopH_);
    nvgFillPaint(vg, nvgImagePattern(vg, ox, oy, scaledW, scaledH, 0, wallpaperImage_, 1.0f));
    nvgFill(vg);
  }

  // Hints: bottom-left, under the notepad (the controls window shows the
  // current settings)
  bool pointer = Settings::instance().get<bool>("/controls/gamepad_pointer", false);
  std::vector<std::string> lines = {
    "===== ПЕСОЧНИЦА =====",
    "\uE0E1  выйти из песочницы",
    "\uE0E4+\uE0E5+\uE0E3  настройки стрима",
    "\uE0E4+\uE0E5+\uE0F0  экранная клавиатура",
    "\uE0E4+\uE0E5+\uE0EF  контроллер как мышь",
#ifndef __SWITCH__
    "Esc (3 с)  настройки стрима",
    "Ctrl+Alt  отпустить мышь",
#endif
    "",
    "Масштаб: " + std::to_string(static_cast<int>(std::lround(uiScale_ * 100))) + " %, " +
      std::to_string(static_cast<int>(std::lround(desktopW_ * uiScale_))) + "×" +
      std::to_string(static_cast<int>(std::lround(desktopH_ * uiScale_))),
  };
  if (pointer) {
    lines.push_back("\uE105 курсор   \uE0E0 / \uE0E6 клик   \uE0E7 правый клик");
    lines.push_back("\uE104 прокрутка   \uE0E3 клавиатура");
  }
  float y = taskbarTop() - 16 - (lines.size() - 1) * 20.0f;
  for (const auto &l : lines) {
    text(vg, 40, y, l, kHint, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, 13);
    y += 20;
  }

  if (selecting_) {
    float x = mouseX_ / scale_, cy = mouseY_ / scale_;
    brls::Rect r(
      std::min(x, selectX_),
      std::min(cy, selectY_),
      std::fabs(x - selectX_),
      std::fabs(cy - selectY_)
    );
    fill(vg, r, gray(255, 40));
    stroke(vg, r, kWhite);
  }
}

void PlaygroundDesktop::drawWindow(NVGcontext *vg, int i) {
  const auto &w = windows_[i];
  bool active = active_ == i;
  auto now = clock_type::now();
  if (i == WinMessageBox && now < flashUntil_) // blinks while flashing
    active =
      std::chrono::duration_cast<std::chrono::milliseconds>(flashUntil_ - now).count() / 100 % 2;
  fill(vg, windowRect(w), kBlack);
  stroke(vg, windowRect(w), active ? kWhite : gray(128));

  // Title bar: inverted when active (High Contrast "active title")
  brls::Rect title(w.x + 1, w.y + 1, w.w - 2, kTitleH);
  NVGcolor titleFg = active ? kBlack : kWhite;
  fill(vg, title, active ? kWhite : kBlack);
  text(vg, title.getMinX() + 10, title.getMidY(), w.title, titleFg);

  const Part caption[] = {Part::Close, Part::Maximize, Part::Minimize};
  for (int k = 0; k < (i == WinMessageBox ? 1 : 3); k++) {
    brls::Rect b(title.getMaxX() - (k + 1) * kCaptionW, title.getMinY(), kCaptionW, kTitleH);
    Hit h{caption[k], i, 0};
    bool down = pressed_ == h && hover_ == h;
    NVGcolor fg = titleFg;
    if (down || hover_ == h) {
      fill(vg, b, down ? gray(128) : active ? gray(200) : gray(64));
    }
    float cx = std::round(b.getMidX()), cy = std::round(b.getMidY());
    switch (caption[k]) {
    case Part::Close:
      line(vg, cx - 5, cy - 5, cx + 5, cy + 5, fg);
      line(vg, cx + 5, cy - 5, cx - 5, cy + 5, fg);
      break;
    case Part::Maximize:
      if (w.maximized) {
        stroke(vg, {cx - 5, cy - 3, 8, 8}, fg);
        line(vg, cx - 3, cy - 5, cx + 5, cy - 5, fg);
        line(vg, cx + 5, cy - 5, cx + 5, cy + 3, fg);
      } else {
        stroke(vg, {cx - 5, cy - 5, 10, 10}, fg);
      }
      break;
    default:
      line(vg, cx - 5, cy + 0.5f, cx + 5, cy + 0.5f, fg);
      break;
    }
  }

  auto c = contentRect(w);
  nvgSave(vg);
  nvgIntersectScissor(vg, c.getMinX(), c.getMinY(), c.getWidth(), c.getHeight());
  switch (i) {
  case WinMessageBox:
    drawMessageBox(vg, c);
    break;
  case WinScheme:
    drawSchemeWindow(vg, c);
    break;
  case WinNotepad:
    drawNotepadWindow(vg, c);
    break;
  case WinList:
    drawListWindow(vg, c);
    break;
  case WinControls:
    drawControlsWindow(vg, c);
    break;
  default:
    break;
  }
  nvgRestore(vg);
}

// isDefault: the button Enter presses, drawn with the thicker border
void PlaygroundDesktop::drawPushButton(
  NVGcontext *vg, const brls::Rect &r, const char *label, bool pressed, bool hover, bool isDefault
) {
  fill(vg, r, pressed ? kWhite : hover ? gray(48) : kBlack);
  stroke(vg, r, kWhite, (hover || isDefault) && !pressed ? 2 : 1);
  text(
    vg,
    r.getMidX(),
    r.getMidY(),
    label,
    pressed ? kBlack : kWhite,
    NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE
  );
}

void PlaygroundDesktop::drawNotepadWindow(NVGcontext *vg, const brls::Rect &c) {
  constexpr float kStatusH = 22, kPad = 6;
  brls::Rect area(
    c.getMinX() + kPad,
    c.getMinY() + kPad,
    c.getWidth() - 2 * kPad,
    c.getHeight() - kStatusH - 2 * kPad
  );

  auto now = clock_type::now();
  bool caretOn =
    std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() / 530 %
      2 ==
    0;
  if (text_.empty()) {
    text(
      vg,
      area.getMinX() + 8,
      area.getMinY() + kNotepadLineH / 2,
      "Печатайте на клавиатуре или экранной клавиатуре",
      kHint,
      NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE,
      kNotepadFont
    );
  }

  // The caret is a '|' after the text; the last row stops before it while it
  // blinks off, so the wrapping doesn't jump
  std::string s = text_ + "|";
  nvgFontFaceId(vg, brls::Application::getDefaultFont());
  nvgFontSize(vg, kNotepadFont);
  std::vector<NVGtextRow> rows;
  const char *start = s.c_str(), *end = start + s.size();
  NVGtextRow chunk[32];
  int n;
  while (start < end && (n = nvgTextBreakLines(vg, start, end, area.getWidth(), chunk, 32)) > 0) {
    rows.insert(rows.end(), chunk, chunk + n);
    start = chunk[n - 1].next;
  }
  int visible = std::max(1, static_cast<int>(area.getHeight() / kNotepadLineH));
  int first = std::max(0, static_cast<int>(rows.size()) - visible);
  nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
  nvgFillColor(vg, kWhite);
  for (int r = first; r < static_cast<int>(rows.size()); r++) {
    const char *rowEnd = rows[r].end;
    if (r == static_cast<int>(rows.size()) - 1 && !caretOn && rowEnd > rows[r].start)
      rowEnd--; // the caret
    nvgText(
      vg, area.getMinX(), area.getMinY() + (r - first) * kNotepadLineH, rows[r].start, rowEnd
    );
  }

  // Status bar: layout, locks, keys held
  float sy = c.getMaxY() - kStatusH;
  line(vg, c.getMinX(), sy + 0.5f, c.getMaxX(), sy + 0.5f, gray(128));
  std::string status = russian_ ? "RU" : "EN";
  if (caps_)
    status += "   Caps Lock";
  status += "   Нажаты: ";
  if (heldKeys_.empty()) {
    status += "—";
  } else {
    for (size_t k = 0; k < heldKeys_.size(); k++)
      status += (k ? " + " : "") + keyName(heldKeys_[k]);
  }
  text(vg, c.getMinX() + 8, sy + kStatusH / 2, status, kWhite);
}

void PlaygroundDesktop::drawListWindow(NVGcontext *vg, const brls::Rect &c) {
  auto box = listBox();
  stroke(vg, box, kWhite);
  brls::Rect items(
    box.getMinX() + 1, box.getMinY() + 1, box.getWidth() - 2 - kScrollW, box.getHeight() - 2
  );

  nvgSave(vg);
  nvgIntersectScissor(vg, items.getMinX(), items.getMinY(), items.getWidth(), items.getHeight());
  int firstRow = static_cast<int>(std::floor(listScroll_));
  float offset = (listScroll_ - firstRow) * kRowH;
  for (int r = 0; r <= listVisibleRows() + 1; r++) {
    int item = firstRow + r;
    if (item >= kListItems)
      break;
    brls::Rect row(items.getMinX(), items.getMinY() + r * kRowH - offset, items.getWidth(), kRowH);
    bool selected = item == listSelected_;
    bool hover = hover_ == Hit{Part::ListItem, WinList, item};
    if (selected || hover)
      fill(vg, row, selected ? kWhite : gray(48));
    text(
      vg,
      row.getMinX() + 6,
      row.getMidY(),
      "Элемент " + std::to_string(item + 1),
      selected ? kBlack : kWhite
    );
  }
  nvgRestore(vg);

  // Scroll bar
  float sx = box.getMaxX() - 1 - kScrollW;
  fill(vg, {sx, box.getMinY() + 1, kScrollW, box.getHeight() - 2}, gray(24));
  auto arrow = [&](Part part, float y, bool up) {
    brls::Rect r(sx, y, kScrollW, kScrollW);
    Hit h{part, WinList, 0};
    bool down = pressed_ == h && hover_ == h;
    if (down || hover_ == h)
      fill(vg, r, down ? kWhite : gray(80));
    NVGcolor fg = down ? kBlack : kWhite;
    float cx = r.getMidX(), cy = r.getMidY();
    nvgBeginPath(vg);
    if (up) {
      nvgMoveTo(vg, cx - 4, cy + 2);
      nvgLineTo(vg, cx + 4, cy + 2);
      nvgLineTo(vg, cx, cy - 2);
    } else {
      nvgMoveTo(vg, cx - 4, cy - 2);
      nvgLineTo(vg, cx + 4, cy - 2);
      nvgLineTo(vg, cx, cy + 2);
    }
    nvgClosePath(vg);
    nvgFillColor(vg, fg);
    nvgFill(vg);
  };
  arrow(Part::ListUp, box.getMinY() + 1, true);
  arrow(Part::ListDown, box.getMaxY() - 1 - kScrollW, false);
  auto thumb = listThumb();
  Hit thumbHit{Part::ListThumb, WinList, 0};
  bool dragging = pressed_ == thumbHit && buttons_[pk::MOUSE_LEFT];
  fill(
    vg,
    {thumb.getMinX() + 2, thumb.getMinY(), thumb.getWidth() - 4, thumb.getHeight()},
    dragging             ? kWhite
    : hover_ == thumbHit ? gray(200)
                         : gray(140)
  );

  // Push button (does nothing) and check box
  Hit buttonHit{Part::Button, WinList, 0};
  drawPushButton(
    vg,
    listButton(),
    listSelected_ == 66 ? "Применить" : "Кнопка",
    pressed_ == buttonHit && hover_ == buttonHit,
    hover_ == buttonHit
  );
  Hit checkHit{Part::Check, WinList, 0};
  bool checkDown = pressed_ == checkHit && hover_ == checkHit;
  auto r = listCheck();
  brls::Rect check(r.getMinX(), r.getMidY() - kCheck / 2, kCheck, kCheck);
  fill(vg, check, checkDown ? gray(128) : hover_ == checkHit ? gray(48) : kBlack);
  stroke(vg, check, kWhite);
  if (listChecked_) {
    nvgBeginPath(vg);
    nvgMoveTo(vg, check.getMinX() + 3, check.getMidY());
    nvgLineTo(vg, check.getMinX() + 5.5f, check.getMaxY() - 3.5f);
    nvgLineTo(vg, check.getMaxX() - 3, check.getMinY() + 3.5f);
    nvgStrokeColor(vg, kWhite);
    nvgStrokeWidth(vg, 1.5f);
    nvgStroke(vg);
  }
  text(vg, check.getMaxX() + 6, r.getMidY(), "Флажок", kWhite);
}

void PlaygroundDesktop::drawControlsWindow(NVGcontext *vg, const brls::Rect &c) {
  auto &settings = Settings::instance();
  auto state = [&](Part part, int index, bool &down, bool &hover) {
    Hit h{part, WinControls, index};
    hover = hover_ == h;
    down = pressed_ == h && hover;
  };

  // Radio groups
  for (int g = 0; g < 2; g++) {
    const auto &group = kRadioGroups[g];
    std::string current = settings.get<std::string>(group.key, group.def);
    text(vg, c.getMinX() + 16, radioRect(c, g, 0).getMinY() - 12, group.label, kWhite);
    for (int o = 0; o < 2; o++) {
      bool down, hover;
      state(Part::Radio, g * 2 + o, down, hover);
      auto r = radioRect(c, g, o);
      float cx = r.getMinX() + kCheck / 2, cy = r.getMidY();
      nvgBeginPath(vg);
      nvgCircle(vg, cx, cy, kCheck / 2 - 0.5f);
      nvgFillColor(vg, down ? gray(128) : hover ? gray(48) : kBlack);
      nvgFill(vg);
      nvgStrokeColor(vg, kWhite);
      nvgStrokeWidth(vg, 1);
      nvgStroke(vg);
      if (current == group.values[o]) {
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, 3.5f);
        nvgFillColor(vg, kWhite);
        nvgFill(vg);
      }
      text(vg, r.getMinX() + kCheck + 6, cy, group.names[o], kWhite);
    }
  }

  // Gamepad pointer check box
  {
    bool down, hover;
    state(Part::Check, 0, down, hover);
    auto r = pointerCheckRect(c);
    brls::Rect box(r.getMinX(), r.getMidY() - kCheck / 2, kCheck, kCheck);
    fill(vg, box, down ? gray(128) : hover ? gray(48) : kBlack);
    stroke(vg, box, kWhite);
    if (settings.get<bool>("/controls/gamepad_pointer", false)) {
      nvgBeginPath(vg);
      nvgMoveTo(vg, box.getMinX() + 3, box.getMidY());
      nvgLineTo(vg, box.getMinX() + 5.5f, box.getMaxY() - 3.5f);
      nvgLineTo(vg, box.getMaxX() - 3, box.getMinY() + 3.5f);
      nvgStrokeColor(vg, kWhite);
      nvgStrokeWidth(vg, 1.5f);
      nvgStroke(vg);
    }
    text(vg, box.getMaxX() + 6, r.getMidY(), "Контроллер как мышь", kWhite);
    state(Part::Button, 0, down, hover);
    drawPushButton(vg, schemeButtonRect(c), "Схема управления", down, hover);
  }

  // Sensitivity trackbars: label and value above a channel with ticks at
  // both ends and at 1.0× (the middle)
  for (int i = 0; i < kSliderCount; i++) {
    const auto &sens = *kSliders[i].sensitivity;
    auto r = sliderRect(c, i);
    float value = controls::get(sens);
    text(vg, r.getMinX(), r.getMinY() - 4, kSliders[i].label, kWhite);
    text(
      vg,
      r.getMaxX(),
      r.getMinY() - 4,
      controls::format(value),
      kWhite,
      NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE
    );
    float cy = r.getMinY() + 10;
    float x0 = sliderX(r, 0), x1 = sliderX(r, 1);
    brls::Rect channel(x0 - 2, cy - 2, x1 - x0 + 4, 4);
    fill(vg, channel, gray(48));
    stroke(vg, channel, gray(160));
    for (float t : {0.0f, 0.5f, 1.0f}) {
      float tx = std::round(sliderX(r, t)) + 0.5f;
      line(vg, tx, cy + kTrackbarThumbH / 2 + 1, tx, cy + kTrackbarThumbH / 2 + 4, gray(160));
    }
    bool down, hover;
    state(Part::Slider, i, down, hover);
    bool dragging = sliderDrag_ == i;
    float tx = sliderX(r, controls::toProgress(sens, value));
    brls::Rect thumb(
      tx - kTrackbarThumbW / 2, cy - kTrackbarThumbH / 2, kTrackbarThumbW, kTrackbarThumbH
    );
    fill(vg, thumb, dragging ? kWhite : hover ? gray(200) : gray(140));
  }

  line(
    vg,
    c.getMinX() + 8,
    c.getMinY() + kPadTop - 4.5f,
    c.getMaxX() - 8,
    c.getMinY() + kPadTop - 4.5f,
    gray(128)
  );

  // Pad tester
  for (int i = 0; i < 4; i++) {
    auto r = padSlotRect(c, i);
    bool shown = shownPad_ == i;
    fill(vg, r, shown ? kWhite : hover_ == Hit{Part::PadSlot, WinControls, i} ? gray(48) : kBlack);
    stroke(vg, r, pads_[i].seen || shown ? kWhite : gray(80));
    text(
      vg,
      r.getMidX(),
      r.getMidY(),
      std::to_string(i + 1),
      shown           ? kBlack
      : pads_[i].seen ? kWhite
                      : gray(80),
      NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE
    );
  }
  text(
    vg,
    c.getMinX() + 16 + 4 * 30 + 6,
    c.getMinY() + kPadTop + 20,
    "Виртуальный контроллер Xbox",
    kHint
  );

  const Pad &pad = pads_[shownPad_];
  auto held = [&](uint8_t b) { return (pad.buttons >> b) & 1; };
  auto axis = [&](uint8_t a) { return std::clamp(pad.axes[a] / 32767.0f, -1.0f, 1.0f); };
  float ox = c.getMinX() + std::max(0.0f, (c.getWidth() - 330) / 2);
  float oy = c.getMinY() + kPadTop + 44;

  auto pill = [&](float x, float y, float w, float h, const char *label, bool on) {
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x + 0.5f, y + 0.5f, w - 1, h - 1, h / 2);
    nvgFillColor(vg, on ? kWhite : kBlack);
    nvgFill(vg);
    nvgStrokeColor(vg, kWhite);
    nvgStrokeWidth(vg, 1);
    nvgStroke(vg);
    text(
      vg, x + w / 2, y + h / 2, label, on ? kBlack : kWhite, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, 10
    );
  };
  auto trigger = [&](float x, float y, const char *label, float value) {
    brls::Rect r(x, y, 70, 10);
    fill(vg, {x, y, 70 * std::clamp(value, 0.0f, 1.0f), 10}, kWhite);
    stroke(vg, r, kWhite);
    text(vg, x + 76, y + 5, label, kHint, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, 10);
  };
  auto stick = [&](float cx, float cy, float ax, float ay, bool pressed) {
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, 24);
    nvgStrokeColor(vg, kWhite);
    nvgStrokeWidth(vg, 1);
    nvgStroke(vg);
    nvgBeginPath(vg);
    nvgCircle(vg, cx + ax * 16, cy + ay * 16, 8);
    nvgFillColor(vg, pressed ? kWhite : gray(128));
    nvgFill(vg);
  };

  trigger(ox + 20, oy, "LT", axis(pk::AXIS_Z));
  trigger(ox + 214, oy, "RT", axis(pk::AXIS_RZ));
  pill(ox + 20, oy + 16, 70, 16, "LB", held(pk::PAD_LB));
  pill(ox + 240, oy + 16, 70, 16, "RB", held(pk::PAD_RB));
  pill(ox + 132, oy + 56, 26, 14, "Back", held(pk::PAD_BACK));
  pill(ox + 172, oy + 56, 26, 14, "Start", held(pk::PAD_START));
  stick(ox + 55, oy + 75, axis(pk::AXIS_X), axis(pk::AXIS_Y), held(pk::PAD_LSTICK));
  stick(ox + 220, oy + 135, axis(pk::AXIS_RX), axis(pk::AXIS_RY), held(pk::PAD_RSTICK));

  // D-pad
  float dx = ox + 110, dy = oy + 135;
  const struct {
    uint32_t bit;
    float x, y;
  } dirs[] = {{pk::POV_N, 0, -1}, {pk::POV_S, 0, 1}, {pk::POV_W, -1, 0}, {pk::POV_E, 1, 0}};
  for (const auto &d : dirs) {
    brls::Rect r(dx - 8 + d.x * 16, dy - 8 + d.y * 16, 16, 16);
    fill(vg, r, pad.pov & d.bit ? kWhite : kBlack);
    stroke(vg, r, kWhite);
  }

  // Face buttons, minus the one the labelled B is mapped to: B leaves the
  // playground instead. StreamActivity reports it per pad; until then assume
  // a Switch pad (literal: Xbox B; positional: Xbox A, at B's place)
  uint8_t exitButton = exitPadButton_[shownPad_];
  if (exitButton == kNoButton) {
    bool positional =
      settings.get<std::string>(kRadioGroups[0].key, kRadioGroups[0].def) == "positional";
    exitButton = positional ? pk::PAD_A : pk::PAD_B;
  }
  const struct {
    uint8_t button;
    const char *label;
    float x, y;
  } face[] = {
    {pk::PAD_Y, "Y", 0, -1}, {pk::PAD_B, "B", 1, 0}, {pk::PAD_A, "A", 0, 1}, {pk::PAD_X, "X", -1, 0}
  };
  for (const auto &f : face) {
    if (f.button == exitButton)
      continue;
    float cx = ox + 275 + f.x * 22, cy = oy + 75 + f.y * 22;
    bool on = held(f.button);
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, 10);
    nvgFillColor(vg, on ? kWhite : kBlack);
    nvgFill(vg);
    nvgStrokeColor(vg, kWhite);
    nvgStrokeWidth(vg, 1);
    nvgStroke(vg);
    text(vg, cx, cy, f.label, on ? kBlack : kWhite, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
  }

  char buf[128];
  std::snprintf(
    buf,
    sizeof buf,
    "LX %+.2f  LY %+.2f   RX %+.2f  RY %+.2f",
    axis(pk::AXIS_X),
    axis(pk::AXIS_Y),
    axis(pk::AXIS_RX),
    axis(pk::AXIS_RY)
  );
  text(vg, c.getMinX() + 16, c.getMaxY() - 14, buf, kHint, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, 11);

  if (settings.get<bool>("/controls/gamepad_pointer", false)) {
    // Only the tester: the settings above stay usable
    brls::Rect pad(c.getMinX(), c.getMinY() + kPadTop, c.getWidth(), c.getHeight() - kPadTop);
    fill(vg, pad, gray(0, 215));
    text(
      vg,
      pad.getMidX(),
      pad.getMidY() - 9,
      "Контроллер работает как мышь,",
      kWhite,
      NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE
    );
    text(
      vg,
      pad.getMidX(),
      pad.getMidY() + 9,
      "кнопки геймпада в игру не передаются",
      kWhite,
      NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE
    );
  }
}

void PlaygroundDesktop::drawSchemeWindow(NVGcontext *vg, const brls::Rect &c) {
  if (!schemeImage_ && !schemeLoadFailed_) {
    schemeImage_ =
      nvgCreateImage(vg, (std::string(BRLS_RESOURCES) + "img/gamepad_pointer.png").c_str(), 0);
    schemeLoadFailed_ = !schemeImage_;
  }
  if (!schemeImage_) {
    text(
      vg,
      c.getMidX(),
      c.getMidY(),
      "Не удалось открыть изображение",
      kWhite,
      NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE
    );
    return;
  }
  // Fit, keeping the aspect ratio (maximized, it grows with the window)
  int iw = 1, ih = 1;
  nvgImageSize(vg, schemeImage_, &iw, &ih);
  float k = std::min(c.getWidth() / iw, c.getHeight() / ih);
  float w = iw * k - 20, h = ih * k - 20;
  float x = c.getMidX() - w / 2, y = c.getMidY() - h / 2;
  nvgBeginPath(vg);
  nvgRect(vg, x, y, w, h);
  nvgFillPaint(vg, nvgImagePattern(vg, x, y, w, h, 0, schemeImage_, 1.0f));
  nvgFill(vg);
}

void PlaygroundDesktop::drawMessageBox(NVGcontext *vg, const brls::Rect &c) {
  // Error icon: a filled circle with a cross (red on Windows)
  float cx = c.getMinX() + 26 + kMessageBoxIcon / 2;
  float cy = c.getMinY() + 26 + kMessageBoxIcon / 2;
  nvgBeginPath(vg);
  nvgCircle(vg, cx, cy, kMessageBoxIcon / 2);
  nvgFillColor(vg, kWhite);
  nvgFill(vg);
  line(vg, cx - 6, cy - 6, cx + 6, cy + 6, kBlack, 2.5f);
  line(vg, cx + 6, cy - 6, cx - 6, cy + 6, kBlack, 2.5f);

  float tx = cx + kMessageBoxIcon / 2 + 12;
  nvgFontFaceId(vg, brls::Application::getDefaultFont());
  nvgFontSize(vg, kFont);
  nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
  nvgTextLineHeight(vg, 1.3f);
  nvgFillColor(vg, kWhite);
  nvgTextBox(vg, tx, c.getMinY() + 26 + 1, c.getMaxX() - 24 - tx, kMessageBoxText, nullptr);
  nvgTextLineHeight(vg, 1.0f);

  // Footer band with the default OK button
  float fy = c.getMaxY() - kMessageBoxFooterH;
  line(vg, c.getMinX(), fy + 0.5f, c.getMaxX(), fy + 0.5f, gray(128));
  Hit ok{Part::DialogOk, WinMessageBox, 0};
  drawPushButton(vg, messageBoxOk(c), "OK", pressed_ == ok && hover_ == ok, hover_ == ok, true);
}

void PlaygroundDesktop::drawTaskbar(NVGcontext *vg) {
  float y0 = taskbarTop();
  fill(vg, {0, y0, desktopW_, kTaskbarH}, kBlack);
  line(vg, 0, y0 + 0.5f, desktopW_, y0 + 0.5f, kWhite);

  // Start: a plain 2×2 grid glyph
  Hit startHit{Part::Start, -1, 0};
  if (hover_ == startHit)
    fill(
      vg,
      {0, y0 + 1, kStartW, kTaskbarH - 1},
      pressed_ == startHit && buttons_[pk::MOUSE_LEFT] ? gray(72) : gray(40)
    );
  float gx = kStartW / 2 - 8, gy = y0 + kTaskbarH / 2 - 8;
  for (int k = 0; k < 4; k++)
    fill(vg, {gx + (k % 2) * 9, gy + (k / 2) * 9, 7, 7}, kWhite);

  auto ids = taskbarWindows();
  for (size_t slot = 0; slot < ids.size(); slot++) {
    int i = ids[slot];
    brls::Rect b(kStartW + slot * kTaskButtonW, y0 + 1, kTaskButtonW, kTaskbarH - 1);
    const auto &w = windows_[i];
    bool front = active_ == i && !w.hidden;
    bool hover = hover_ == Hit{Part::TaskbarButton, i, 0};
    if (front || hover)
      fill(vg, b, front ? gray(72) : gray(40));
    text(vg, b.getMinX() + 12, b.getMidY(), w.title, kWhite);
    if (!w.hidden)
      fill(vg, {b.getMinX() + 4, b.getMaxY() - 2, b.getWidth() - 8, 2}, front ? kWhite : gray(150));
  }

  // Tray, right to left: clock, cursor position, layout, exit hint
  char clockBuf[8];
  std::time_t t = std::time(nullptr);
  std::strftime(clockBuf, sizeof clockBuf, "%H:%M", std::localtime(&t));
  char posBuf[32];
  // Physical pixels, like GetCursorPos() on a DPI-aware app
  std::snprintf(
    posBuf,
    sizeof posBuf,
    "%d, %d",
    static_cast<int>(mouseX_ / scale_ * uiScale_),
    static_cast<int>(mouseY_ / scale_ * uiScale_)
  );
  const std::string tray[] = {clockBuf, posBuf, russian_ ? "RU" : "EN", "\uE0E1 Выход"};
  float x = desktopW_ - 12;
  float my = y0 + kTaskbarH / 2;
  for (const auto &s : tray) {
    text(vg, x, my, s, kWhite, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
    x -= textWidth(vg, s) + 20;
  }
}

void PlaygroundDesktop::drawContextMenu(NVGcontext *vg) {
  brls::Rect menu(menuX_, menuY_, kMenuW, 4 + kMenuCount * kMenuItemH);
  fill(vg, menu, kBlack);
  stroke(vg, menu, kWhite);
  for (int i = 0; i < kMenuCount; i++) {
    brls::Rect item(menuX_ + 2, menuY_ + 2 + i * kMenuItemH, kMenuW - 4, kMenuItemH);
    bool hover = hover_ == Hit{Part::MenuItem, -1, i};
    if (hover)
      fill(vg, item, kWhite);
    text(vg, item.getMinX() + 26, item.getMidY(), kMenuItems[i], hover ? kBlack : kWhite);
  }
}

} // namespace vkpcnx
