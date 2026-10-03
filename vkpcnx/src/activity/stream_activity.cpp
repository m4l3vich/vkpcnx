#include "activity/stream_activity.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "core/controls.hpp"
#include "core/settings.hpp"
#include "core/stream/sdp_munging.hpp"
#include "core/utils/display.hpp"
#include "view/side_panel.hpp"

#if defined(__GLFW__) && !defined(__SWITCH__)
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <borealis/platforms/glfw/glfw_video.hpp>
#define VKPCNX_HAS_GLFW_KEYNAMES 1
#define VKPCNX_HAS_GLFW_CLIPBOARD 1
#define VKPCNX_HAS_GLFW_GAMEPADS 1
#define VKPCNX_HAS_GLFW_CONTENT_SCALE 1
#endif

using namespace vkpcnx;
using namespace vkpcnx::stream;

// SC_LEFT_GAME_TIME / SC_SUBSCRIPTION_*_LEFT don't have an explicit "no
// limit" flag (protocol doc §5.6) — an account/game without a real cap just
// reports a huge count_seconds (observed: ~10 years). No real session timer
// runs anywhere close to this, so treat it as unlimited rather than print it.
static constexpr int64_t kUnlimitedTimeLeftThresholdSeconds = 7LL * 24 * 3600;

// ---- key mapping (§9.1): GLFW key code → DirectInput scan code ----------------

static uint8_t glfwKeyToDik(int key) {
  using K = brls::BrlsKeyboardScancode;
  switch (key) {
  case K::BRLS_KBD_KEY_ESCAPE:
    return 1;
  case K::BRLS_KBD_KEY_1:
    return 2;
  case K::BRLS_KBD_KEY_2:
    return 3;
  case K::BRLS_KBD_KEY_3:
    return 4;
  case K::BRLS_KBD_KEY_4:
    return 5;
  case K::BRLS_KBD_KEY_5:
    return 6;
  case K::BRLS_KBD_KEY_6:
    return 7;
  case K::BRLS_KBD_KEY_7:
    return 8;
  case K::BRLS_KBD_KEY_8:
    return 9;
  case K::BRLS_KBD_KEY_9:
    return 10;
  case K::BRLS_KBD_KEY_0:
    return 11;
  case K::BRLS_KBD_KEY_MINUS:
    return 12;
  case K::BRLS_KBD_KEY_EQUAL:
    return 13;
  case K::BRLS_KBD_KEY_BACKSPACE:
    return 14;
  case K::BRLS_KBD_KEY_TAB:
    return 15;
  case K::BRLS_KBD_KEY_Q:
    return 16;
  case K::BRLS_KBD_KEY_W:
    return 17;
  case K::BRLS_KBD_KEY_E:
    return 18;
  case K::BRLS_KBD_KEY_R:
    return 19;
  case K::BRLS_KBD_KEY_T:
    return 20;
  case K::BRLS_KBD_KEY_Y:
    return 21;
  case K::BRLS_KBD_KEY_U:
    return 22;
  case K::BRLS_KBD_KEY_I:
    return 23;
  case K::BRLS_KBD_KEY_O:
    return 24;
  case K::BRLS_KBD_KEY_P:
    return 25;
  case K::BRLS_KBD_KEY_LEFT_BRACKET:
    return 26;
  case K::BRLS_KBD_KEY_RIGHT_BRACKET:
    return 27;
  case K::BRLS_KBD_KEY_ENTER:
    return 28;
  case K::BRLS_KBD_KEY_LEFT_CONTROL:
    return 29;
  case K::BRLS_KBD_KEY_A:
    return 30;
  case K::BRLS_KBD_KEY_S:
    return 31;
  case K::BRLS_KBD_KEY_D:
    return 32;
  case K::BRLS_KBD_KEY_F:
    return 33;
  case K::BRLS_KBD_KEY_G:
    return 34;
  case K::BRLS_KBD_KEY_H:
    return 35;
  case K::BRLS_KBD_KEY_J:
    return 36;
  case K::BRLS_KBD_KEY_K:
    return 37;
  case K::BRLS_KBD_KEY_L:
    return 38;
  case K::BRLS_KBD_KEY_SEMICOLON:
    return 39;
  case K::BRLS_KBD_KEY_APOSTROPHE:
    return 40;
  case K::BRLS_KBD_KEY_GRAVE_ACCENT:
    return 41;
  case K::BRLS_KBD_KEY_LEFT_SHIFT:
    return 42;
  case K::BRLS_KBD_KEY_BACKSLASH:
    return 43;
  case K::BRLS_KBD_KEY_WORLD_1:
    return 43; // IntlBackslash
  case K::BRLS_KBD_KEY_Z:
    return 44;
  case K::BRLS_KBD_KEY_X:
    return 45;
  case K::BRLS_KBD_KEY_C:
    return 46;
  case K::BRLS_KBD_KEY_V:
    return 47;
  case K::BRLS_KBD_KEY_B:
    return 48;
  case K::BRLS_KBD_KEY_N:
    return 49;
  case K::BRLS_KBD_KEY_M:
    return 50;
  case K::BRLS_KBD_KEY_COMMA:
    return 51;
  case K::BRLS_KBD_KEY_PERIOD:
    return 52;
  case K::BRLS_KBD_KEY_SLASH:
    return 53;
  case K::BRLS_KBD_KEY_RIGHT_SHIFT:
    return 54;
  case K::BRLS_KBD_KEY_KP_MULTIPLY:
    return 55;
  case K::BRLS_KBD_KEY_LEFT_ALT:
    return 56;
  case K::BRLS_KBD_KEY_SPACE:
    return 57;
  case K::BRLS_KBD_KEY_CAPS_LOCK:
    return 58;
  case K::BRLS_KBD_KEY_F1:
    return 59;
  case K::BRLS_KBD_KEY_F2:
    return 60;
  case K::BRLS_KBD_KEY_F3:
    return 61;
  case K::BRLS_KBD_KEY_F4:
    return 62;
  case K::BRLS_KBD_KEY_F5:
    return 63;
  case K::BRLS_KBD_KEY_F6:
    return 64;
  case K::BRLS_KBD_KEY_F7:
    return 65;
  case K::BRLS_KBD_KEY_F8:
    return 66;
  case K::BRLS_KBD_KEY_F9:
    return 67;
  case K::BRLS_KBD_KEY_F10:
    return 68;
  case K::BRLS_KBD_KEY_NUM_LOCK:
    return 69;
  case K::BRLS_KBD_KEY_SCROLL_LOCK:
    return 70;
  case K::BRLS_KBD_KEY_KP_7:
    return 71;
  case K::BRLS_KBD_KEY_KP_8:
    return 72;
  case K::BRLS_KBD_KEY_KP_9:
    return 73;
  case K::BRLS_KBD_KEY_KP_SUBTRACT:
    return 74;
  case K::BRLS_KBD_KEY_KP_4:
    return 75;
  case K::BRLS_KBD_KEY_KP_5:
    return 76;
  case K::BRLS_KBD_KEY_KP_6:
    return 77;
  case K::BRLS_KBD_KEY_KP_ADD:
    return 78;
  case K::BRLS_KBD_KEY_KP_1:
    return 79;
  case K::BRLS_KBD_KEY_KP_2:
    return 80;
  case K::BRLS_KBD_KEY_KP_3:
    return 81;
  case K::BRLS_KBD_KEY_KP_0:
    return 82;
  case K::BRLS_KBD_KEY_KP_DECIMAL:
    return 83;
  case K::BRLS_KBD_KEY_WORLD_2:
    return 86;
  case K::BRLS_KBD_KEY_PRINT_SCREEN:
    return 86; // as the web client does
  case K::BRLS_KBD_KEY_F11:
    return 87;
  case K::BRLS_KBD_KEY_F12:
    return 88;
  case K::BRLS_KBD_KEY_F13:
    return 100;
  case K::BRLS_KBD_KEY_F14:
    return 101;
  case K::BRLS_KBD_KEY_F15:
    return 102;
  case K::BRLS_KBD_KEY_KP_EQUAL:
    return 141;
  case K::BRLS_KBD_KEY_KP_ENTER:
    return 156;
  case K::BRLS_KBD_KEY_RIGHT_CONTROL:
    return 157;
  case K::BRLS_KBD_KEY_KP_DIVIDE:
    return 181;
  case K::BRLS_KBD_KEY_RIGHT_ALT:
    return 184;
  case K::BRLS_KBD_KEY_PAUSE:
    return 197;
  case K::BRLS_KBD_KEY_HOME:
    return 199;
  case K::BRLS_KBD_KEY_UP:
    return 200;
  case K::BRLS_KBD_KEY_PAGE_UP:
    return 201;
  case K::BRLS_KBD_KEY_LEFT:
    return 203;
  case K::BRLS_KBD_KEY_RIGHT:
    return 205;
  case K::BRLS_KBD_KEY_END:
    return 207;
  case K::BRLS_KBD_KEY_DOWN:
    return 208;
  case K::BRLS_KBD_KEY_PAGE_DOWN:
    return 209;
  case K::BRLS_KBD_KEY_INSERT:
    return 210;
  case K::BRLS_KBD_KEY_DELETE:
    return 211;
  case K::BRLS_KBD_KEY_LEFT_SUPER:
    return 219;
  case K::BRLS_KBD_KEY_RIGHT_SUPER:
    return 220;
  case K::BRLS_KBD_KEY_MENU:
    return 221;
  default:
    return pk::KEY_UNASSIGNED;
  }
}

// Locale of the current keyboard layout for a letter key ("ru-RU"/"en-US"),
// or "" when it can't be determined (modifiers, navigation keys, Switch)
static std::string localeForKey(int key) {
  if (key < brls::BRLS_KBD_KEY_A || key > brls::BRLS_KBD_KEY_Z)
    return "";
#ifdef VKPCNX_HAS_GLFW_KEYNAMES
  const char *name = glfwGetKeyName(key, 0);
  if (name && name[0]) {
    unsigned char c0 = static_cast<unsigned char>(name[0]);
    unsigned char c1 = static_cast<unsigned char>(name[1]);
    // Cyrillic block U+0400–U+04FF is D0 80 … D3 BF in UTF-8
    if ((c0 == 0xD0 || c0 == 0xD1) && c1 >= 0x80)
      return "ru-RU";
    if (c0 < 0x80)
      return "en-US";
  }
#endif
  return "en-US";
}

// Whether gamepad slot `slot` (< 4) holds a pad. GLFW joystick ids are stable,
// so one pad disconnecting doesn't renumber the others; borealis only reports
// a count there. libnx pads are compacted anyway.
static bool padSlotConnected(int slot, int count) {
#ifdef VKPCNX_HAS_GLFW_GAMEPADS
  (void)count;
  return glfwJoystickIsGamepad(GLFW_JOYSTICK_1 + slot);
#else
  return slot < count;
#endif
}

// The client's UI scale, 1 = 100 %: the OS display scale on desktop (Retina,
// Windows / GNOME scaling), borealis' content scale on Switch (the 1280×720 UI
// is drawn at 1.5× when docked)
static float systemUiScale() {
#ifdef VKPCNX_HAS_GLFW_CONTENT_SCALE
  if (auto *ctx = dynamic_cast<brls::GLFWVideoContext *>(
        brls::Application::getPlatform()->getVideoContext()
      )) {
    float xs = 1, ys = 1;
    glfwGetWindowContentScale(ctx->getGLFWWindow(), &xs, &ys);
    if (ys > 0)
      return ys;
  }
#endif
#ifdef __SWITCH__
  return brls::Application::windowScale;
#else
  return static_cast<float>(brls::Application::getPlatform()->getVideoContext()->getScaleFactor());
#endif
}

// The borealis button a pad's physical, labelled B arrives as. libnx reports
// buttons by label, so that's BUTTON_B on Switch. GLFW's (SDL) mappings are
// positional, Xbox layout: on a Nintendo pad the labelled B sits at the bottom
// and arrives as BUTTON_A. Other desktop pads keep B at the right.
static brls::ControllerButton labelledBButton(int slot) {
#ifdef VKPCNX_HAS_GLFW_GAMEPADS
  // SDL-style GUID: characters 8-11 are the USB vendor id, little-endian
  const char *guid = glfwGetJoystickGUID(GLFW_JOYSTICK_1 + slot);
  const char *name = glfwGetGamepadName(GLFW_JOYSTICK_1 + slot);
  bool nintendo = (guid && std::strlen(guid) >= 12 && std::strncmp(guid + 8, "7e05", 4) == 0) ||
                  (name && (std::strstr(name, "Nintendo") || std::strstr(name, "Joy-Con")));
  if (nintendo)
    return brls::BUTTON_A;
#else
  (void)slot;
#endif
  return brls::BUTTON_B;
}

// Gamepad pointer mode: buttons sent as keys (DirectInput scan codes)
static const struct {
  brls::ControllerButton button;
  uint8_t dik;
} kPointerKeys[] = {
  {brls::BUTTON_UP, 200},
  {brls::BUTTON_DOWN, 208},
  {brls::BUTTON_LEFT, 203},
  {brls::BUTTON_RIGHT, 205},
  {brls::BUTTON_START, 28}, // Enter
  {brls::BUTTON_BACK, 1},   // Esc
  {brls::BUTTON_X, 14},     // Backspace
};

// ---- activity ------------------------------------------------------------------

StreamActivity::StreamActivity(GameCard::Game game, PlayUrl playUrl)
    : game_(std::move(game)), playUrl_(std::move(playUrl)) {}

StreamActivity::StreamActivity(Playground) : playgroundMode_(true) {}

InputSink &StreamActivity::sink() {
  if (playground_)
    return *playground_;
  return session_->input();
}

bool StreamActivity::live() const {
  if (playground_)
    return playgroundStarted_; // not before its first capture (pollInputs)
  return session_ && session_->isStreaming();
}

StreamActivity::~StreamActivity() {
  setCaptured(false);
  auto *input = brls::Application::getPlatform()->getInputManager();
  if (haveKeySub_) {
    input->getKeyboardKeyStateChanged()->unsubscribe(keySub_);
    input->getMouseCusorOffsetChanged()->unsubscribe(mouseSub_);
    input->getMouseScrollOffsetChanged()->unsubscribe(scrollSub_);
    brls::Application::getWindowFocusChangedEvent()->unsubscribe(focusSub_);
  }
  if (hudTimer_)
    brls::cancelDelay(hudTimer_);
  session_.reset();
}

StreamSession::Config StreamActivity::buildConfig() const {
  auto &settings = Settings::instance();
  StreamSession::Config c;
  c.playUrl = playUrl_;

  auto [monitorW, monitorH] = vkpcnx::utils::displayPixelSize();
  c.manager.monitorWidth = monitorW;
  c.manager.monitorHeight = monitorH;
#ifdef __SWITCH__
  // The Switch can't take in UDP much faster than ~1500 packets/s: at the
  // server's default 25+ Mbit/s cap a third of the video packets were lost
  constexpr int DEFAULT_BITRATE_MAX_MBIT = 12;
#else
  constexpr int DEFAULT_BITRATE_MAX_MBIT = 0; // server default (§6.3)
#endif
  c.manager.bitrateMaxMbit =
    settings.get<int>("/stream/bitrate_max_mbit", DEFAULT_BITRATE_MAX_MBIT);
  c.manager.bitrateMinMbit = settings.get<int>("/stream/bitrate_min_mbit", 0);

  // §6.3: stream size = min(user resolution, monitor) snapped to the list
  int wantW = settings.get<int>("/stream/width", 0), wantH = settings.get<int>("/stream/height", 0);
  if (playUrl_.resolutionWidth > 0) {
    wantW = playUrl_.resolutionWidth;
    wantH = playUrl_.resolutionHeight;
  }
  int w = wantW > 0 ? std::min(wantW, monitorW) : monitorW;
  int h = wantH > 0 ? std::min(wantH, monitorH) : monitorH;
  auto [sw, sh] = snapStreamResolution(w, h);
  brls::Logger::info("Stream: display {}x{} px, stream {}x{}", monitorW, monitorH, sw, sh);
  int fps = playUrl_.fps > 0 ? playUrl_.fps : settings.get<int>("/stream/fps", 0);
#ifdef __SWITCH__
  int maxFps = 60; // 60 Hz screen; more only costs bandwidth
#else
  int maxFps = 120;
#endif
  if (fps > maxFps)
    fps = maxFps;
#ifdef __SWITCH__
  if (fps <= 0)
    fps = maxFps; // "auto" lets the server pick up to 120
#endif

  c.video.fps = fps;
  c.video.streamWidth = sw;
  c.video.streamHeight = sh;
  c.video.rgbRange = settings.get<int>("/stream/rgb_range", 0);
  std::string profile = settings.get<std::string>("/stream/h264_profile", "auto");
  c.video.profile = profile == "high"              ? H264Profile::High
                    : profile == "main"            ? H264Profile::Main
                    : profile == "base"            ? H264Profile::Base
                    : profile == "constrainedhigh" ? H264Profile::ConstrainedHigh
                    : profile == "constrainedbase" ? H264Profile::ConstrainedBase
                                                   : H264Profile::Auto;
  auto br = defaultBitrates(sw, sh, fps > 0 ? fps : 60);
  c.video.bitrates.startKbps = br.startKbps;
  c.video.bitrates.minKbps =
    c.manager.bitrateMinMbit > 0 ? c.manager.bitrateMinMbit * 1000 : br.minKbps;
  c.video.bitrates.maxKbps =
    c.manager.bitrateMaxMbit > 0 ? c.manager.bitrateMaxMbit * 1000 : br.maxKbps;
  return c;
}

void StreamActivity::onContentAvailable() {
  if (playgroundMode_) {
    // Controls playground: a desktop as tall as the stream would be, at the
    // Windows display scale matching ours, takes the input; captured on the
    // first frame with a video rectangle (pollInputs)
    playground_ =
      std::make_unique<PlaygroundDesktop>(buildConfig().video.streamHeight, systemUiScale());
    video->onDrawBackdrop = [this](NVGcontext *vg, const brls::Rect &rect) {
      playground_->draw(vg, rect);
    };
    overlay->setVisibility(brls::Visibility::GONE);
  } else {
    setupSession();
  }
  setupInput();
  if (session_)
    session_->start(buildConfig());
}

void StreamActivity::setupSession() {
  session_ = std::make_unique<StreamSession>();
  auto *session = session_.get();

  video->setDecoder(&session->decoder());
  video->onFirstFrame = [this] { session_->notifyVideoStarted(); };
  video->onVideoRectChanged = [this](int w, int h) { session_->setVideoRect(w, h); };

  session->onStateChange = [this](StreamSession::State state) {
    switch (state) {
    case StreamSession::State::ConnectingManager:
      statusText->setText("Подключение к серверу-шлюзу…");
      break;
    case StreamSession::State::ConnectingGameServer: {
      const auto &gs = session_->gameServerAddress();
      statusText->setText(
        "Подключение к игровому серверу (сессия " + std::to_string(gs.sessionId) + ")…"
      );
      break;
    }
    case StreamSession::State::Negotiating:
      statusText->setText(std::string("Установка WebRTC-соединения…"));
      overlay->setVisibility(brls::Visibility::VISIBLE);
      setCaptured(false);
      break;
    case StreamSession::State::Streaming:
      if (!sessionStartedAt_)
        sessionStartedAt_ = std::chrono::steady_clock::now();
      overlay->setVisibility(brls::Visibility::GONE);
      if (!dialogOpen_) {
        brls::Application::notify("Нажмите \uE0E4+\uE0E5+\uE0E3 для открытия настроек стрима");
        setCaptured(true);
      }
      break;
    case StreamSession::State::Reconnecting:
      statusText->setText("Сигнальный WebSocket игрового сервера разорван, переподключение…");
      overlay->setVisibility(brls::Visibility::VISIBLE);
      setCaptured(false);
      break;
    case StreamSession::State::Ended:
      if (!leaving_ && !dialogOpen_)
        leave();
      break;
    default:
      break;
    }
  };
  session->onStatus = [this](const std::string &header, const std::string &text, int progress) {
    std::string t = text;
    if (progress >= 0)
      t += " (" + std::to_string(progress) + "%)";
    if (!t.empty())
      statusText->setText(t);
  };
  session->onNotification = [](const std::string &m) { brls::Application::notify(m); };
  session->onError = [this](const SessionError &e) {
    // Not every source logs before reporting; what the user saw belongs in the log
    brls::Logger::warning(
      "Stream: session error kind={} state={}: {}{}",
      static_cast<int>(e.kind),
      static_cast<int>(session_->state()),
      e.title.empty() ? "" : e.title + ": ",
      e.message
    );
    if (session_->state() == StreamSession::State::Failed ||
        e.kind == SessionError::Kind::SessionTimeout ||
        e.kind == SessionError::Kind::DuplicateClient) {
      showErrorAndLeave(e.title.empty() ? "Ошибка сессии" : e.title, e.message);
    } else {
      brls::Application::notify(e.message);
    }
  };
  session->onSessionEnd = [this](GameServerClient::EndReason) {
    if (!leaving_)
      leave();
  };
  session->onTimeLeft = [this](int64_t seconds) {
    lastTimeLeftSeconds_ = seconds;
    if (seconds >= kUnlimitedTimeLeftThresholdSeconds) {
      timer->setVisibility(brls::Visibility::GONE);
      return;
    }
    int64_t h = seconds / 3600, m = (seconds / 60) % 60, s = seconds % 60;
    char buf[32];
    if (h > 0)
      std::snprintf(
        buf, sizeof buf, "%lld:%02lld:%02lld", (long long)h, (long long)m, (long long)s
      );
    else
      std::snprintf(buf, sizeof buf, "%02lld:%02lld", (long long)m, (long long)s);
    timer->setText(buf);
    timer->setVisibility(brls::Visibility::VISIBLE);
  };
  session->onTimerDisabled = [this] { timer->setVisibility(brls::Visibility::GONE); };
  session->onCursor = [this](const CursorImage &c) {
    cursorShown_ = !c.rgba.empty();
    brls::Logger::debug(
      "Stream: cursor {} {} ({}x{}), captured={}",
      c.id,
      cursorShown_ ? "shown" : "hidden",
      c.width,
      c.height,
      captured_
    );
    video->setCursor(c);
    video->setCursorVisible(captured_ && cursorShown_);
  };
  session->onOpenUrl = [](const std::string &url) {
    brls::Logger::info("Stream: open URL {}", url);
  };
  session->onVmName = [this](const std::string &name) { gameServerName_ = name; };
  session->onClipboardText = [this](const std::string &text) {
    lastClipboard_ = text;
#ifdef VKPCNX_HAS_GLFW_CLIPBOARD
    if (auto *ctx = dynamic_cast<brls::GLFWVideoContext *>(
          brls::Application::getPlatform()->getVideoContext()
        ))
      glfwSetClipboardString(ctx->getGLFWWindow(), text.c_str());
#endif
  };
  session->onSessionEvent = [](const GameServerClient::SessionEvent &ev) {
    if (ev.type == "native")
      brls::Application::notify(ev.name);
  };
}

void StreamActivity::setupInput() {
  // On-screen keyboard
  osk->onKey = [this](uint8_t dik, bool pressed, const std::string &locale) {
    if (hasSink())
      sink().keyEvent(dik, pressed, locale);
  };
  osk->onCapsChanged = [this](bool on) {
    oskLockKeys_ = on ? pk::LOCK_CAPS : 0;
    if (hasSink())
      sink().setLockKeys(oskLockKeys_);
  };
  osk->onClose = [this] { setOskOpen(false); };
  osk->onDockToggle = [this] {
    oskDockTop_ = !oskDockTop_;
    placeOsk();
  };

  // Input plumbing
  auto *input = brls::Application::getPlatform()->getInputManager();
  keySub_ = input->getKeyboardKeyStateChanged()->subscribe([this](brls::KeyState k) { onKey(k); });
  mouseSub_ = input->getMouseCusorOffsetChanged()->subscribe([this](brls::Point offset) {
    if (!captured_)
      return;
    int dx = static_cast<int>(offset.x), dy = static_cast<int>(offset.y);
    if (dx == 0 && dy == 0)
      return;
    auto rect = video->videoRect();
    cursorX_ = std::clamp(cursorX_ + dx, 0.0f, rect.getWidth());
    cursorY_ = std::clamp(cursorY_ + dy, 0.0f, rect.getHeight());
    video->setCursorPosition(cursorX_, cursorY_);
    sink().mouseMove(dx, dy);
  });
  scrollSub_ = input->getMouseScrollOffsetChanged()->subscribe([this](brls::Point offset) {
    if (!captured_ || offset.y == 0)
      return;
    // GLFW reports 1.0 per wheel notch; trackpads and smooth wheels send
    // fractions, which add up to whole notches. A direction change restarts.
    if ((wheelAccum_ > 0) != (offset.y > 0))
      wheelAccum_ = 0;
    wheelAccum_ += offset.y;
    while (std::fabs(wheelAccum_) >= 1.0f) {
      int step = wheelAccum_ > 0 ? 1 : -1;
      sink().mouseWheel(step);
      wheelAccum_ -= step;
    }
  });
  focusSub_ = brls::Application::getWindowFocusChangedEvent()->subscribe([this](bool focused) {
    if (!focused)
      lctrlDown_ = laltDown_ = escDown_ = false; // releases may never arrive
    if (!focused && captured_)
      setCaptured(false);
    else if (focused && !captured_ && !dialogOpen_ && !overlayOpen_ && live() && !leaving_)
      setCaptured(true);
  });
  haveKeySub_ = true;
  video->onDraw = [this] { pollInputs(); };
}

void StreamActivity::willAppear(bool resetState) { brls::Activity::willAppear(resetState); }

// borealis calls onResume() (not willAppear) on the activity underneath when
// the one on top -- the stream overlay's SidePanel -- is popped.
void StreamActivity::onResume() {
  brls::Activity::onResume();
  if (overlayOpen_) {
    overlayOpen_ = false;
    if (live() && !dialogOpen_ && !leaving_)
      setCaptured(true);
  }
}

void StreamActivity::willDisappear(bool resetState) {
  setCaptured(false);
  brls::Activity::willDisappear(resetState);
}

void StreamActivity::setCaptured(bool captured) {
  if (captured == captured_)
    return;
  captured_ = captured;
  auto *input = brls::Application::getPlatform()->getInputManager();
  if (captured) {
    releasedByUser_ = false;
    brls::Application::blockInputs(true);
    input->setPointerLock(true);
    // Whatever is held right now (the click that captured, the button that
    // closed the overlay) was meant for the client UI, not the game
    input->updateMouseStates(&lastMouse_);
    suppressHeldPadButtons();
#ifdef VKPCNX_HAS_GLFW_CLIPBOARD
    // borealis' GLFW cursorCallback measures offsets from the window centre but
    // never warps there on lock, so the first event after re-locking would
    // report a jump from wherever the cursor was left while the overlay was up
    if (auto *ctx = dynamic_cast<brls::GLFWVideoContext *>(
          brls::Application::getPlatform()->getVideoContext()
        )) {
      int w, h;
      glfwGetWindowSize(ctx->getGLFWWindow(), &w, &h);
      glfwSetCursorPos(ctx->getGLFWWindow(), w / 2, h / 2);
    }
#endif
    // Keep the cursor where it was: the server-side absolute position is
    // untouched by an overlay/focus round-trip, so re-centring here would
    // desync the drawn cursor from the game's. Only the first capture starts
    // at the centre (matching InputChannel::setWindowSize).
    auto rect = video->videoRect();
    if (!cursorInit_) {
      cursorInit_ = true;
      cursorX_ = rect.getWidth() / 2;
      cursorY_ = rect.getHeight() / 2;
    }
    cursorX_ = std::clamp(cursorX_, 0.0f, rect.getWidth());
    cursorY_ = std::clamp(cursorY_, 0.0f, rect.getHeight());
    video->setCursorPosition(cursorX_, cursorY_);
    video->setCursorVisible(cursorShown_);
    if (hasSink()) {
      sink().setFocus(true);
#ifdef VKPCNX_HAS_GLFW_CLIPBOARD
      // §7.7: the clipboard is pushed to the server on every focus gain
      if (auto *ctx = dynamic_cast<brls::GLFWVideoContext *>(
            brls::Application::getPlatform()->getVideoContext()
          )) {
        const char *text = glfwGetClipboardString(ctx->getGLFWWindow());
        if (text && *text && std::string(text) != lastClipboard_) {
          lastClipboard_ = text;
          sink().sendClipboardText(lastClipboard_);
        }
      }
#endif
    }
  } else {
    video->setCursorVisible(false);
    // Releases go out before the focus loss (which releases everything
    // server-side anyway); this resets what the keyboard shows as held
    osk->releaseAll();
    oskFingers_.clear();
    otherFingers_.clear();
    oskFocusDx_ = oskFocusDy_ = 0;
    for (bool &b : oskPadPrev_)
      b = false;
    if (hasSink())
      sink().setFocus(false);
    input->setPointerLock(false);
    brls::Application::unblockInputs();
    for (auto &pad : padButtons_)
      for (bool &b : pad)
        b = false;
    for (auto &p : padPov_)
      p = 0;
    for (bool &b : pointerButtons_)
      b = false;
    for (bool &k : pointerKeys_)
      k = false;
    pointerOskPrev_ = false;
    pointerScrollAccum_ = 0;
    pointerHoldSeconds_ = 0;
    wheelAccum_ = 0;
    cursorRemX_ = cursorRemY_ = 0;
    touch_ = TouchGesture{};
    tapClickPending_ = false;
    escHoldSeconds_ = 0;
  }
}

void StreamActivity::suppressHeldPadButtons() {
  auto *input = brls::Application::getPlatform()->getInputManager();
  int padCount = input->getControllersConnectedCount();
  for (int i = 0; i < 4; i++) {
    if (!padSlotConnected(i, padCount))
      continue;
    brls::ControllerState st{};
    input->updateControllerState(&st, i);
    for (int b = 0; b < brls::_BUTTON_MAX; b++)
      padSuppressed_[i][b] = padSuppressed_[i][b] || st.buttons[b];
  }
}

void StreamActivity::onKey(brls::KeyState key) {
  // Track keys for the client-side hotkeys even when not captured. Only the
  // left modifiers: AltGr arrives as right Alt (+ left Ctrl on Windows).
  if (key.key == brls::BRLS_KBD_KEY_LEFT_CONTROL)
    lctrlDown_ = key.pressed;
  if (key.key == brls::BRLS_KBD_KEY_LEFT_ALT)
    laltDown_ = key.pressed;
  if (key.key == brls::BRLS_KBD_KEY_ESCAPE)
    escDown_ = key.pressed;

  if (!captured_ || !hasSink())
    return;

#ifndef __SWITCH__
  // Ctrl+Alt releases the mouse (and every held key) back to the desktop
  if (key.pressed && lctrlDown_ && laltDown_) {
    setCaptured(false);
    releasedByUser_ = true;
    brls::Application::notify("Мышь освобождена. Кликните по окну, чтобы вернуть управление");
    return;
  }
#endif

#ifdef __APPLE__
  // §9.1: Meta is never sent on macOS
  if (key.key == brls::BRLS_KBD_KEY_LEFT_SUPER || key.key == brls::BRLS_KBD_KEY_RIGHT_SUPER)
    return;
#endif
  uint8_t dik = glfwKeyToDik(key.key);
  if (dik == pk::KEY_UNASSIGNED)
    return;
  uint8_t lock = 0;
  if (key.mods & 0x0010)
    lock |= pk::LOCK_CAPS; // GLFW_MOD_CAPS_LOCK
  if (key.mods & 0x0020)
    lock |= pk::LOCK_NUM; // GLFW_MOD_NUM_LOCK
  sink().setLockKeys(lock);
  sink().keyEvent(dik, key.pressed, localeForKey(key.key));
}

void StreamActivity::pollInputs() {
  if (hudVisible_)
    updateHud();
  logStats();
  auto *input = brls::Application::getPlatform()->getInputManager();

  if (playgroundMode_ && !playgroundStarted_ && !leaving_ && video->videoRect().getWidth() > 0) {
    playgroundStarted_ = true;
    brls::Application::notify("\uE0E1 — выход, \uE0E4+\uE0E5+\uE0E3 — настройки управления");
    setCaptured(true);
  }

  // Released with Ctrl+Alt: a left click in the window captures again
  if (!captured_ && releasedByUser_) {
    brls::RawMouseState mouse;
    input->updateMouseStates(&mouse);
    bool clicked = mouse.leftButton && !lastMouse_.leftButton;
    lastMouse_ = mouse;
    if (clicked && live() && !dialogOpen_ && !overlayOpen_ && !leaving_)
      setCaptured(true);
    return;
  }

  if (!captured_ || !hasSink())
    return;
  auto &ch = sink();

  // borealis only polls the platform (Switch hid keyboard/mouse, GLFW pointer
  // buffers) from processInput(), which is skipped while inputs are blocked
  input->runloopStart();

  // Mouse buttons (§9.2)
  brls::RawMouseState mouse;
  input->updateMouseStates(&mouse);
  if (mouse.leftButton != lastMouse_.leftButton)
    ch.mouseButton(pk::MOUSE_LEFT, mouse.leftButton);
  if (mouse.rightButton != lastMouse_.rightButton)
    ch.mouseButton(pk::MOUSE_RIGHT, mouse.rightButton);
  if (mouse.middleButton != lastMouse_.middleButton)
    ch.mouseButton(pk::MOUSE_MIDDLE, mouse.middleButton);
  lastMouse_ = mouse;

  // Frame time for the analog (stick / touch) cursor helpers
  auto now = std::chrono::steady_clock::now();
  float dt = lastPollAt_.time_since_epoch().count()
               ? std::chrono::duration<float>(now - lastPollAt_).count()
               : 0.0f;
  lastPollAt_ = now;
  dt = std::clamp(dt, 0.0f, 0.1f);

  PadAction action = pollGamepads(input, dt);
  pollTouch(input, dt);

  // Hold Esc for 3 s to open the stream overlay. Esc itself reaches the game;
  // everything held is released when the overlay takes the input.
  escHoldSeconds_ = escDown_ ? escHoldSeconds_ + dt : 0;
  if (escHoldSeconds_ >= 3.0f) {
    escHoldSeconds_ = 0;
    showStreamOverlay();
    return;
  }

  switch (action) {
  case PadAction::Overlay:
    showStreamOverlay();
    break;
  case PadAction::Keyboard:
    setOskOpen(!oskOpen_);
    break;
  case PadAction::TogglePointer: {
    auto &settings = Settings::instance();
    bool on = !settings.get<bool>("/controls/gamepad_pointer", false);
    settings.set("/controls/gamepad_pointer", on);
    brls::Application::notify(on ? "Контроллер как мышь: вкл" : "Контроллер как мышь: выкл");
    break;
  }
  case PadAction::Exit:
    leave();
    break;
  case PadAction::None:
    break;
  }
}

StreamActivity::PadAction StreamActivity::pollGamepads(brls::InputManager *input, float dt) {
  auto &ch = sink();
  auto &settings = Settings::instance();

  // Gamepads (§9.3): all treated as XInput, index < 4.
  // "positional" mapping (overlay setting) sends the button at the same
  // physical position on the emulated Xbox pad, i.e. A<->B and X<->Y swapped.
  const bool positional =
    settings.get<std::string>("/controls/mapping_mode", "positional") == "positional";
  // "gamepad pointer": pads drive the mouse and send no gamepad events at all
  const bool pointerMode = settings.get<bool>("/controls/gamepad_pointer", false);

  // Mode switched (hotkey or overlay): drop whatever the other mode holds.
  // The virtual pads' axes survive "release everything" (§8.6), so they are
  // zeroed explicitly.
  if (pointerMode != lastPointerMode_) {
    lastPointerMode_ = pointerMode;
    if (pointerMode) {
      for (int i = 0; i < 4; i++)
        if (padConnected_[i])
          releasePad(i);
    } else {
      releasePointerInputs();
    }
  }

  // Client-side hotkeys (web client §9.3 + overlay): L+R and one more button,
  // on the press of that button. The chord is then held back from the game
  // (L/R get releases) until each of its buttons is let go.
  struct Hotkey {
    brls::ControllerButton button;
    PadAction action;
  };
  static const Hotkey hotkeys[] = {
    {brls::BUTTON_Y, PadAction::Overlay},
    {brls::BUTTON_BACK, PadAction::Keyboard},       // Minus on Switch
    {brls::BUTTON_START, PadAction::TogglePointer}, // Plus on Switch
  };
  PadAction action = PadAction::None;

  int count = input->getControllersConnectedCount();
  bool connected[4];
  bool anyConnected = false;
  for (int i = 0; i < 4; i++) {
    connected[i] = padSlotConnected(i, count);
    anyConnected = anyConnected || connected[i];
  }

  brls::ControllerState merged{}; // pointer mode / keyboard: all pads act as one
  for (int i = 0; i < 4; i++) {
    if (!connected[i]) {
      if (padConnected_[i]) {
        padConnected_[i] = false;
        for (bool &b : padSuppressed_[i])
          b = false;
        if (anyConnected) {
          releasePad(i);
        } else {
          // §9.3: the last pad going away releases everything
          for (bool &b : padButtons_[i])
            b = false;
          padPov_[i] = 0;
          for (bool &b : pointerButtons_)
            b = false;
          for (bool &k : pointerKeys_)
            k = false;
          ch.releaseAll();
        }
      }
      continue;
    }
    padConnected_[i] = true;
    brls::ControllerState st{};
    input->updateControllerState(&st, i);
    if (st.buttons[brls::BUTTON_LB] && st.buttons[brls::BUTTON_RB])
      oskShoulderChord_ = true; // seen before the hotkey suppression masks them

    auto &suppressed = padSuppressed_[i];
    for (int b = 0; b < brls::_BUTTON_MAX; b++)
      if (!st.buttons[b])
        suppressed[b] = false;
    if (st.buttons[brls::BUTTON_LB] && st.buttons[brls::BUTTON_RB]) {
      for (const auto &h : hotkeys) {
        if (st.buttons[h.button] && !suppressed[h.button]) {
          suppressed[brls::BUTTON_LB] = suppressed[brls::BUTTON_RB] = true;
          suppressed[h.button] = true;
          if (action == PadAction::None)
            action = h.action;
          break;
        }
      }
    }
    for (int b = 0; b < brls::_BUTTON_MAX; b++)
      if (suppressed[b])
        st.buttons[b] = false;
    // Playground: the labelled B leaves, whatever the mapping mode, in place
    // of the right click / pad button (the on-screen keyboard keeps it)
    const brls::ControllerButton exitButton = labelledBButton(i);
    if (playgroundMode_ && !oskOpen_ && st.buttons[exitButton]) {
      suppressed[exitButton] = true;
      st.buttons[exitButton] = false;
      if (action == PadAction::None)
        action = PadAction::Exit;
    }

    if (pointerMode || oskOpen_) {
      for (int b = 0; b < brls::_BUTTON_MAX; b++)
        merged.buttons[b] = merged.buttons[b] || st.buttons[b];
      for (int a = 0; a < brls::_AXES_MAX; a++)
        merged.axes[a] = std::clamp(merged.axes[a] + st.axes[a], -1.0f, 1.0f);
      continue;
    }

    struct Map {
      brls::ControllerButton brls;
      uint8_t pk;
    };
    static const Map literalButtons[] = {
      {brls::BUTTON_A, pk::PAD_A},
      {brls::BUTTON_B, pk::PAD_B},
      {brls::BUTTON_X, pk::PAD_X},
      {brls::BUTTON_Y, pk::PAD_Y},
      {brls::BUTTON_LB, pk::PAD_LB},
      {brls::BUTTON_RB, pk::PAD_RB},
      {brls::BUTTON_BACK, pk::PAD_BACK},
      {brls::BUTTON_START, pk::PAD_START},
      {brls::BUTTON_LSB, pk::PAD_LSTICK},
      {brls::BUTTON_RSB, pk::PAD_RSTICK},
    };
    static const Map positionalButtons[] = {
      {brls::BUTTON_A, pk::PAD_B},
      {brls::BUTTON_B, pk::PAD_A},
      {brls::BUTTON_X, pk::PAD_Y},
      {brls::BUTTON_Y, pk::PAD_X},
      {brls::BUTTON_LB, pk::PAD_LB},
      {brls::BUTTON_RB, pk::PAD_RB},
      {brls::BUTTON_BACK, pk::PAD_BACK},
      {brls::BUTTON_START, pk::PAD_START},
      {brls::BUTTON_LSB, pk::PAD_LSTICK},
      {brls::BUTTON_RSB, pk::PAD_RSTICK},
    };

    // Axes first, then POV, then buttons (per-frame order of §9.3)
    auto scale = [](float v) -> int32_t {
      v = std::clamp(v, -1.0f, 1.0f);
      return static_cast<int32_t>(v > 0 ? v * 32767 : v * 32768);
    };
#ifdef __SWITCH__
    float lt = st.buttons[brls::BUTTON_LT] ? 1.0f : 0.0f;
    float rt = st.buttons[brls::BUTTON_RT] ? 1.0f : 0.0f;
#else
    float lt = (st.axes[brls::LEFT_Z] + 1.0f) / 2.0f;
    float rt = (st.axes[brls::RIGHT_Z] + 1.0f) / 2.0f;
    if (st.buttons[brls::BUTTON_LT] && lt < 0.1f)
      lt = 1.0f;
    if (st.buttons[brls::BUTTON_RT] && rt < 0.1f)
      rt = 1.0f;
#endif
    ch.gamepadAxis(i, pk::AXIS_X, scale(st.axes[brls::LEFT_X]));
    ch.gamepadAxis(i, pk::AXIS_Y, scale(st.axes[brls::LEFT_Y]));
    ch.gamepadAxis(i, pk::AXIS_RX, scale(st.axes[brls::RIGHT_X]));
    ch.gamepadAxis(i, pk::AXIS_RY, scale(st.axes[brls::RIGHT_Y]));
    ch.gamepadAxis(i, pk::AXIS_Z, scale(lt));
    ch.gamepadAxis(i, pk::AXIS_RZ, scale(rt));

    uint32_t pov = 0;
    if (st.buttons[brls::BUTTON_UP])
      pov |= pk::POV_N;
    if (st.buttons[brls::BUTTON_DOWN])
      pov |= pk::POV_S;
    if (st.buttons[brls::BUTTON_LEFT])
      pov |= pk::POV_W;
    if (st.buttons[brls::BUTTON_RIGHT])
      pov |= pk::POV_E;
    if (pov != padPov_[i]) {
      padPov_[i] = pov;
      ch.gamepadPov(i, pov);
    }

    const Map *buttons = positional ? positionalButtons : literalButtons;
    for (size_t k = 0; k < std::size(literalButtons); k++) {
      const Map &m = buttons[k];
      if (playground_ && m.brls == exitButton) // the tester leaves that one out
        playground_->setExitPadButton(i, m.pk);
      bool pressed = st.buttons[m.brls];
      if (pressed != padButtons_[i][m.pk]) {
        padButtons_[i][m.pk] = pressed;
        ch.gamepadButton(i, m.pk, pressed);
      }
    }
  }

  if (oskOpen_ && anyConnected)
    pollOskGamepad(merged, dt);
  else if (pointerMode && anyConnected)
    pollGamepadPointer(merged, dt, action);
  return action;
}

void StreamActivity::releasePad(int index) {
  auto &ch = sink();
  for (uint8_t b = 0; b < 16; b++) {
    if (padButtons_[index][b]) {
      padButtons_[index][b] = false;
      ch.gamepadButton(index, b, false);
    }
  }
  if (padPov_[index]) {
    padPov_[index] = 0;
    ch.gamepadPov(index, 0);
  }
  for (uint8_t axis = pk::AXIS_X; axis <= pk::AXIS_RZ; axis++)
    ch.gamepadAxis(index, axis, 0);
}

void StreamActivity::releasePointerInputs() {
  auto &ch = sink();
  for (uint8_t b = 0; b < std::size(pointerButtons_); b++) {
    if (pointerButtons_[b]) {
      pointerButtons_[b] = false;
      ch.mouseButton(b, false);
    }
  }
  for (size_t k = 0; k < std::size(kPointerKeys); k++) {
    if (pointerKeys_[k]) {
      pointerKeys_[k] = false;
      ch.keyEvent(kPointerKeys[k].dik, false);
    }
  }
  pointerOskPrev_ = false;
  pointerScrollAccum_ = 0;
  pointerHoldSeconds_ = 0;
}

// Stick dead zone + squared response for fine control near the centre
static float shapeStick(float v) {
  constexpr float kDeadZone = 0.2f;
  float a = std::fabs(v);
  if (a < kDeadZone)
    return 0.0f;
  float n = (a - kDeadZone) / (1.0f - kDeadZone);
  return std::copysign(n * n, v);
}

void StreamActivity::stickCursor(float x, float y, float dt) {
  constexpr float kCursorSpeed = 1000.0f; // video-rect px/s at full deflection
  // Acceleration: held at full deflection, the speed ramps up to kMaxBoost×
  // over kBoostRampSec so crossing the screen doesn't take forever
  constexpr float kFullDeflection = 0.9f;
  constexpr float kMaxBoost = 2.5f;
  constexpr float kBoostRampSec = 1.0f;

  float mx = shapeStick(x), my = shapeStick(y);
  if (std::hypot(x, y) >= kFullDeflection)
    pointerHoldSeconds_ += dt;
  else
    pointerHoldSeconds_ = 0;
  float boost = 1.0f + (kMaxBoost - 1.0f) * std::min(pointerHoldSeconds_ / kBoostRampSec, 1.0f);
  float speed = kCursorSpeed * controls::get(controls::kGamepadCursor) * boost;
  if (mx != 0 || my != 0)
    moveCursorBy(mx * speed * dt, my * speed * dt);
}

void StreamActivity::setPointerButton(uint8_t button, bool pressed) {
  if (pointerButtons_[button] == pressed)
    return;
  pointerButtons_[button] = pressed;
  sink().mouseButton(button, pressed);
}

// Gamepad-as-mouse, all pads merged: right stick moves the cursor, left stick
// scrolls, A or ZL = left button (ZL is the one to drag with, the right thumb
// is on the stick), B or ZR = right button, left stick press = middle button,
// D-pad = arrow keys, Plus (Start) = Enter, Minus (Back) = Esc, X = Backspace,
// Y = on-screen keyboard.
void StreamActivity::pollGamepadPointer(
  const brls::ControllerState &st, float dt, PadAction &action
) {
  constexpr float kScrollNotchesPerSec = 10.0f;

  stickCursor(st.axes[brls::RIGHT_X], st.axes[brls::RIGHT_Y], dt);

  // Stick up (negative Y in borealis) = wheel up (+1 notch)
  float sy = shapeStick(st.axes[brls::LEFT_Y]);
  pointerScrollAccum_ += -sy * kScrollNotchesPerSec * controls::get(controls::kGamepadScroll) * dt;
  while (std::fabs(pointerScrollAccum_) >= 1.0f) {
    int step = pointerScrollAccum_ > 0 ? 1 : -1;
    sink().mouseWheel(step);
    pointerScrollAccum_ -= step;
  }
  if (sy == 0)
    pointerScrollAccum_ = 0;

  // Triggers as buttons: borealis reports them pressed past 10% on desktop
  const bool mouseButtons[] = {
    st.buttons[brls::BUTTON_A] || st.buttons[brls::BUTTON_LT], // MOUSE_LEFT
    st.buttons[brls::BUTTON_B] || st.buttons[brls::BUTTON_RT], // MOUSE_RIGHT
    st.buttons[brls::BUTTON_LSB],                              // MOUSE_MIDDLE
  };
  for (uint8_t b = 0; b < std::size(mouseButtons); b++)
    setPointerButton(b, mouseButtons[b]);
  for (size_t k = 0; k < std::size(kPointerKeys); k++) {
    bool pressed = st.buttons[kPointerKeys[k].button];
    if (pressed != pointerKeys_[k]) {
      pointerKeys_[k] = pressed;
      sink().keyEvent(kPointerKeys[k].dik, pressed);
    }
  }

  bool osk = st.buttons[brls::BUTTON_Y];
  if (osk && !pointerOskPrev_ && action == PadAction::None)
    action = PadAction::Keyboard;
  pointerOskPrev_ = osk;
}

// On-screen keyboard open, all pads merged: D-pad / left stick move the focus
// (repeating while held), A presses the focused key, B = Backspace, X = Shift,
// Y = Space, Plus = Enter, a tap of L / R = language / Fn layer, Minus closes.
// The right stick moves the mouse, ZL / ZR click left / right.
void StreamActivity::pollOskGamepad(const brls::ControllerState &st, float dt) {
  using Action = vkpcnx::OnscreenKeyboard::Action;
  constexpr float kStickThreshold = 0.5f;
  constexpr float kRepeatDelay = 0.4f, kRepeatInterval = 0.08f;

  auto pressed = [&](brls::ControllerButton b) { return st.buttons[b] && !oskPadPrev_[b]; };
  auto released = [&](brls::ControllerButton b) { return !st.buttons[b] && oskPadPrev_[b]; };

  if (pressed(brls::BUTTON_BACK)) {
    setOskOpen(false);
    return;
  }

  int dx = 0, dy = 0;
  if (st.buttons[brls::BUTTON_LEFT])
    dx = -1;
  else if (st.buttons[brls::BUTTON_RIGHT])
    dx = 1;
  else if (st.buttons[brls::BUTTON_UP])
    dy = -1;
  else if (st.buttons[brls::BUTTON_DOWN])
    dy = 1;
  else {
    float sx = st.axes[brls::LEFT_X], sy = st.axes[brls::LEFT_Y];
    if (std::max(std::fabs(sx), std::fabs(sy)) >= kStickThreshold) {
      if (std::fabs(sx) > std::fabs(sy))
        dx = sx > 0 ? 1 : -1;
      else
        dy = sy > 0 ? 1 : -1; // stick down is positive Y in borealis
    }
  }
  if (dx != oskFocusDx_ || dy != oskFocusDy_) {
    oskFocusDx_ = dx;
    oskFocusDy_ = dy;
    if (dx != 0 || dy != 0) {
      osk->moveFocus(dx, dy);
      oskFocusRepeat_ = kRepeatDelay;
    }
  } else if (dx != 0 || dy != 0) {
    oskFocusRepeat_ -= dt;
    if (oskFocusRepeat_ <= 0) {
      osk->moveFocus(dx, dy);
      oskFocusRepeat_ += kRepeatInterval;
    }
  }

  if (pressed(brls::BUTTON_A))
    osk->pressFocused(true);
  else if (released(brls::BUTTON_A))
    osk->pressFocused(false);

  static const struct {
    brls::ControllerButton button;
    Action action;
  } kActions[] = {
    {brls::BUTTON_B, Action::Backspace},
    {brls::BUTTON_X, Action::Shift},
    {brls::BUTTON_Y, Action::Space},
    {brls::BUTTON_START, Action::Enter},
  };
  for (const auto &a : kActions) {
    if (pressed(a.button))
      osk->pressAction(a.action, true);
    else if (released(a.button))
      osk->pressAction(a.action, false);
  }

  // L / R act on release, and only when the other one wasn't held meanwhile:
  // L+R is the start of the client hotkeys (L+R+Minus closes the keyboard)
  static const struct {
    brls::ControllerButton button, other;
    Action action;
  } kShoulders[] = {
    {brls::BUTTON_LB, brls::BUTTON_RB, Action::Language},
    {brls::BUTTON_RB, brls::BUTTON_LB, Action::FnLayer},
  };
  for (const auto &s : kShoulders) {
    if (pressed(s.button) && !st.buttons[s.other])
      oskShoulderChord_ = false;
    if (released(s.button) && !oskShoulderChord_) {
      osk->pressAction(s.action, true);
      osk->pressAction(s.action, false);
    }
  }

  stickCursor(st.axes[brls::RIGHT_X], st.axes[brls::RIGHT_Y], dt);
  setPointerButton(pk::MOUSE_LEFT, st.buttons[brls::BUTTON_LT]);
  setPointerButton(pk::MOUSE_RIGHT, st.buttons[brls::BUTTON_RT]);

  std::copy(std::begin(st.buttons), std::end(st.buttons), std::begin(oskPadPrev_));
}

// Touchscreen → mouse, per /controls/touchscreen_mode:
//  trackpad:    one finger drags the cursor (with acceleration), tap = left
//               click, tap then touch-and-move = left-button drag, two-finger
//               tap = right click
//  touchscreen: the cursor jumps to the finger; tap = left click, hold =
//               right click, drag = left-button drag
// Both: two fingers moving vertically scroll the wheel.
void StreamActivity::pollTouch(brls::InputManager *input, float dt) {
  using clock = std::chrono::steady_clock;
  constexpr float kTapSlop = 12.0f; // window points
  constexpr float kScrollPxPerNotch = 40.0f;
  constexpr auto kTapMax = std::chrono::milliseconds(300);
  constexpr auto kLongPress = std::chrono::milliseconds(600);
  // Trackpad acceleration: gain grows linearly with finger speed (points/s)
  // from kSlowGain at kSlowSpeed to kFastGain at kFastSpeed
  constexpr float kSlowGain = 1.0f, kFastGain = 3.5f;
  constexpr float kSlowSpeed = 150.0f, kFastSpeed = 1500.0f;

  std::vector<brls::RawTouchState> touches;
  input->updateTouchStates(&touches);
  touches.erase(
    std::remove_if(touches.begin(), touches.end(), [](auto &t) { return !t.pressed; }),
    touches.end()
  );

  // Fingers landing on the on-screen keyboard type on it; the rest keep
  // driving the mouse below
  if (oskOpen_) {
    auto has = [](const std::vector<int> &v, int id) {
      return std::find(v.begin(), v.end(), id) != v.end();
    };
    std::vector<int> onOsk, elsewhere;
    std::vector<brls::RawTouchState> rest;
    for (const auto &t : touches) {
      bool mine = has(oskFingers_, t.fingerId);
      if (mine) {
        osk->touchMove(t.fingerId, t.position);
      } else if (!has(otherFingers_, t.fingerId)) {
        // New finger. The floating keyboard button stays usable on top.
        bool onButton = oskButtonVisible_ && oskButton->getFrame().pointInside(t.position);
        mine = !onButton && osk->touchDown(t.fingerId, t.position);
      }
      (mine ? onOsk : elsewhere).push_back(t.fingerId);
      if (!mine)
        rest.push_back(t);
    }
    std::vector<int> lifted;
    for (int id : oskFingers_)
      if (!has(onOsk, id))
        lifted.push_back(id);
    oskFingers_ = std::move(onOsk);
    otherFingers_ = std::move(elsewhere);
    touches = std::move(rest);
    for (int id : lifted) {
      osk->touchUp(id);
      if (!oskOpen_)
        break; // its close button was released
    }
  }

  auto &ch = sink();
  const bool touchscreen =
    Settings::instance().get<std::string>("/controls/touchscreen_mode", "touchscreen") ==
    "touchscreen";
  auto now = clock::now();
  int fingers = static_cast<int>(touches.size());

  // A tap's left button is released once the tap-drag window passes
  if (tapClickPending_ && fingers == 0 && now >= tapClickReleaseAt_) {
    tapClickPending_ = false;
    ch.mouseButton(pk::MOUSE_LEFT, false);
  }

  if (fingers == 0) {
    if (!touch_.active)
      return;
    if (touch_.onOskButton) {
      bool tapped = !touch_.moved;
      touch_ = TouchGesture{};
      if (tapped)
        setOskOpen(!oskOpen_);
      return;
    }
    // Gesture ended: decide what the tap meant
    bool quick = now - touch_.downAt <= kTapMax;
    if (touch_.leftHeld) {
      touch_.leftHeld = false;
      ch.mouseButton(pk::MOUSE_LEFT, false);
      // Second quick tap of a double tap: the button went down with the
      // first one and just came up, so this is the second click
      if (touch_.tapDrag && touch_.maxFingers == 1 && quick && !touch_.moved)
        tapClick();
    } else if (touch_.maxFingers == 1 && !touch_.rightSent && touchscreen) {
      clickMouse(pk::MOUSE_LEFT);
    } else if (!touchscreen && touch_.maxFingers == 1 && quick && !touch_.moved) {
      tapClick();
    } else if (touch_.maxFingers == 2 && !touchscreen && quick && !touch_.moved) {
      clickMouse(pk::MOUSE_RIGHT);
    }
    touch_ = TouchGesture{};
    return;
  }

  brls::Point primary = touches[0].position;
  brls::Point centroid{0, 0};
  for (auto &t : touches) {
    centroid.x += t.position.x / fingers;
    centroid.y += t.position.y / fingers;
  }

  if (!touch_.active) {
    touch_ = TouchGesture{};
    touch_.active = true;
    touch_.downAt = now;
    touch_.start = touch_.last = primary;
    touch_.lastCentroid = centroid;
    touch_.maxFingers = fingers;
    // A finger landing on the on-screen keyboard button drags it (or taps it)
    // instead of driving the stream cursor
    if (oskButtonVisible_ && fingers == 1) {
      auto frame = oskButton->getFrame();
      if (frame.pointInside(primary)) {
        touch_.onOskButton = true;
        touch_.oskGrab = {primary.x - frame.getMinX(), primary.y - frame.getMinY()};
        if (tapClickPending_) {
          tapClickPending_ = false;
          ch.mouseButton(pk::MOUSE_LEFT, false);
        }
        return;
      }
    }
    if (tapClickPending_) {
      // Touching again right after a tap keeps its button down: a drag
      tapClickPending_ = false;
      if (!touchscreen && fingers == 1) {
        touch_.leftHeld = true;
        touch_.tapDrag = true;
      } else {
        ch.mouseButton(pk::MOUSE_LEFT, false);
      }
    }
    if (touchscreen && fingers == 1) {
      auto rect = video->videoRect();
      moveCursorTo(primary.x - rect.getMinX(), primary.y - rect.getMinY());
    }
    return;
  }

  if (touch_.onOskButton) {
    if (!touch_.moved &&
        std::hypot(primary.x - touch_.start.x, primary.y - touch_.start.y) > kTapSlop)
      touch_.moved = true;
    if (touch_.moved)
      placeOskButton(primary.x - touch_.oskGrab.x, primary.y - touch_.oskGrab.y);
    return;
  }

  if (fingers > touch_.maxFingers) {
    touch_.maxFingers = fingers;
    touch_.lastCentroid = centroid; // don't scroll on the jump of the centroid
    if (touch_.leftHeld) {          // second finger cancels a drag
      touch_.leftHeld = false;
      ch.mouseButton(pk::MOUSE_LEFT, false);
    }
  }

  float dx = primary.x - touch_.last.x, dy = primary.y - touch_.last.y;
  touch_.last = primary;
  if (!touch_.moved &&
      std::hypot(primary.x - touch_.start.x, primary.y - touch_.start.y) > kTapSlop)
    touch_.moved = true;

  if (fingers >= 2) {
    // Two-finger vertical drag = wheel. Trackpad follows the Windows touchpad
    // default (fingers down = scroll down); touchscreen drags the content.
    float cdy = centroid.y - touch_.lastCentroid.y;
    touch_.lastCentroid = centroid;
    touch_.scrollAccum += (touchscreen ? cdy : -cdy) / kScrollPxPerNotch;
    while (std::fabs(touch_.scrollAccum) >= 1.0f) {
      int step = touch_.scrollAccum > 0 ? 1 : -1;
      ch.mouseWheel(step);
      touch_.scrollAccum -= step;
    }
    return;
  }

  if (touch_.maxFingers > 1)
    return; // a finger lifted mid-scroll: ignore the remaining one

  if (!touchscreen) {
    if (dt > 0) {
      // Smoothed: per-frame touch deltas are too jittery on their own
      float speed = std::hypot(dx, dy) / dt;
      touch_.speed = touch_.speed * 0.5f + speed * 0.5f;
    }
    float t = std::clamp((touch_.speed - kSlowSpeed) / (kFastSpeed - kSlowSpeed), 0.0f, 1.0f);
    float gain = (kSlowGain + (kFastGain - kSlowGain) * t) * controls::get(controls::kTrackpad);
    if (dx != 0 || dy != 0)
      moveCursorBy(dx * gain, dy * gain);
    return;
  }

  // touchscreen mode
  if (touch_.moved) {
    if (!touch_.leftHeld && !touch_.rightSent) {
      touch_.leftHeld = true;
      ch.mouseButton(pk::MOUSE_LEFT, true);
    }
    auto rect = video->videoRect();
    moveCursorTo(primary.x - rect.getMinX(), primary.y - rect.getMinY());
  } else if (!touch_.rightSent && now - touch_.downAt >= kLongPress) {
    touch_.rightSent = true;
    clickMouse(pk::MOUSE_RIGHT);
  }
}

void StreamActivity::moveCursorBy(float dx, float dy) {
  cursorRemX_ += dx;
  cursorRemY_ += dy;
  int ix = static_cast<int>(cursorRemX_), iy = static_cast<int>(cursorRemY_);
  if (ix == 0 && iy == 0)
    return;
  cursorRemX_ -= ix;
  cursorRemY_ -= iy;
  auto rect = video->videoRect();
  cursorX_ = std::clamp(cursorX_ + ix, 0.0f, rect.getWidth());
  cursorY_ = std::clamp(cursorY_ + iy, 0.0f, rect.getHeight());
  video->setCursorPosition(cursorX_, cursorY_);
  sink().mouseMove(ix, iy);
}

void StreamActivity::moveCursorTo(float x, float y) {
  auto rect = video->videoRect();
  cursorX_ = std::clamp(x, 0.0f, rect.getWidth());
  cursorY_ = std::clamp(y, 0.0f, rect.getHeight());
  cursorRemX_ = cursorRemY_ = 0;
  video->setCursorPosition(cursorX_, cursorY_);
  sink().mouseMoveTo(static_cast<int>(cursorX_), static_cast<int>(cursorY_));
}

void StreamActivity::clickMouse(uint8_t button) {
  sink().mouseButton(button, true);
  sink().mouseButton(button, false);
}

void StreamActivity::tapClick() {
  constexpr auto kTapDragWindow = std::chrono::milliseconds(250);
  sink().mouseButton(pk::MOUSE_LEFT, true);
  tapClickPending_ = true;
  tapClickReleaseAt_ = std::chrono::steady_clock::now() + kTapDragWindow;
}

void StreamActivity::updateHud() {
  if (!session_)
    return;
  auto s = session_->stats();
  char buf[256];
  char rtt[32];
  if (s.rtt)
    std::snprintf(rtt, sizeof rtt, "%lldms", (long long)s.rtt->count());
  else
    std::snprintf(rtt, sizeof rtt, "-");
  std::snprintf(
    buf,
    sizeof buf,
    "%.0ffps rtt:%s input_rtt:%.1fms decode:%.1fms err:%llu dropped:%llu video:%s/%s",
    video->renderedFps(),
    rtt,
    s.inputRttMs,
    s.decoder.avgDecodeMs,
    (unsigned long long)s.decoder.decodeErrors,
    (unsigned long long)s.decoder.framesDropped,
    s.decoder.codecName.c_str(),
    s.decoder.hwAccelName ? s.decoder.hwAccelName->c_str() : "software"
  );
  hud->setText(buf);
}

void StreamActivity::logStats() {
  if (!session_ || !session_->isStreaming()) {
    lastLoggedStats_.reset();
    return;
  }
  auto now = std::chrono::steady_clock::now();
  if (lastLoggedStats_ && now - statsLoggedAt_ < kStatsLogInterval)
    return;
  double seconds = std::chrono::duration<double>(now - statsLoggedAt_).count();
  statsLoggedAt_ = now;
  auto s = session_->stats();
  if (!lastLoggedStats_) {
    lastLoggedStats_ = s;
    return;
  }
  // Counters restart when the video connection is renegotiated
  auto delta = [](uint64_t cur, uint64_t prev) { return cur >= prev ? cur - prev : cur; };
  const auto &p = *lastLoggedStats_;
  uint64_t frames = delta(s.decoder.framesDecoded, p.decoder.framesDecoded);
  uint64_t bytes = delta(s.videoBytes, p.videoBytes);
  uint64_t received = delta(s.rtpPacketsReceived, p.rtpPacketsReceived);
  uint64_t lost = delta(s.rtpPacketsLost, p.rtpPacketsLost);
  brls::Logger::debug(
    "Stats: render {:.1f} fps, decode {:.1f} fps, {:.2f} Mbit/s, rtt {}, input rtt {:.1f} ms, "
    "decode {:.1f} ms, rtp lost {}/{}, decode errors +{}, dropped +{}, keyframe req +{}, "
    "audio buffered {}, {}x{} {}/{}",
    video->renderedFps(),
    frames / seconds,
    bytes * 8 / seconds / 1e6,
    s.rtt ? std::to_string(s.rtt->count()) + " ms" : std::string("-"),
    s.inputRttMs,
    s.decoder.avgDecodeMs,
    lost,
    received + lost,
    delta(s.decoder.decodeErrors, p.decoder.decodeErrors),
    delta(s.decoder.framesDropped, p.decoder.framesDropped),
    delta(s.keyframeRequests, p.keyframeRequests),
    s.audioBufferedFrames,
    s.decoder.width,
    s.decoder.height,
    s.decoder.codecName,
    s.decoder.hwAccelName.value_or("software")
  );
  lastLoggedStats_ = s;
}

void StreamActivity::showStreamOverlay() {
  if (dialogOpen_ || leaving_ || overlayOpen_)
    return;
  overlayOpen_ = true;
  setOskOpen(false);
  setCaptured(false);
  auto *content = new vkpcnx::StreamOverlay();
  content->setSessionMetrics(collectSessionMetrics()); // avoid a blank first frame
  content->setMetricsProvider([this] { return collectSessionMetrics(); });
  content->setDebugOverlayVisible(hudVisible_);
  if (playgroundMode_)
    content->setPlaygroundMode();
  content->onToggleDebugOverlay = [this, content] {
    setHudVisible(!hudVisible_);
    content->setDebugOverlayVisible(hudVisible_);
  };
  content->setOskButtonVisible(oskButtonVisible_);
  content->onToggleOskButton = [this, content] {
    setOskButtonVisible(!oskButtonVisible_);
    content->setOskButtonVisible(oskButtonVisible_);
  };
  auto *panel = new vkpcnx::SidePanel(content, 480);
  content->onOpenOnscreenKeyboard = [this, panel] {
    // popActivity() runs onResume() (which re-captures input) synchronously;
    // the callback fires once the panel has faded out
    panel->dismiss([this] { setOskOpen(true); });
  };
  content->onEndSession = [this, panel] {
    // Close the panel first so willAppear() doesn't re-capture input over
    // the exit dialog; leave() is guarded against re-entry.
    panel->dismiss([this] { leave(); });
  };
  panel->setCancelable(true);
  panel->open();
}

void StreamActivity::setHudVisible(bool visible) {
  hudVisible_ = visible;
  hudBox->setVisibility(hudVisible_ ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
}

// The button is "gone" until first shown, so it has no laid-out size yet:
// fall back to the size from stream.xml
static constexpr float kOskButtonSize = 56.0f;

static float oskButtonExtent(float laidOut) { return laidOut > 0 ? laidOut : kOskButtonSize; }

void StreamActivity::setOskButtonVisible(bool visible) {
  oskButtonVisible_ = visible;
  if (visible && !oskButtonPlaced_) {
    // First show: bottom-left corner, out of the way of the HUD and the timer
    constexpr float kMargin = 24.0f;
    float rootH = root->getHeight() > 0 ? root->getHeight() : brls::Application::contentHeight;
    placeOskButton(kMargin, rootH - oskButtonExtent(oskButton->getHeight()) - kMargin);
  }
  oskButton->setVisibility(oskButtonVisible_ ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
  placeOsk();
}

void StreamActivity::placeOskButton(float left, float top) {
  float maxLeft = std::max(0.0f, root->getWidth() - oskButtonExtent(oskButton->getWidth()));
  float maxTop = std::max(0.0f, root->getHeight() - oskButtonExtent(oskButton->getHeight()));
  oskButtonLeft_ = std::clamp(left - root->getX(), 0.0f, maxLeft);
  oskButtonTop_ = std::clamp(top - root->getY(), 0.0f, maxTop);
  oskButton->setPositionLeft(oskButtonLeft_);
  oskButton->setPositionTop(oskButtonTop_);
  oskButtonPlaced_ = true;
}

// Height from stream.xml, until the keyboard has been laid out once
static constexpr float kOskHeight = 300.0f;

void StreamActivity::setOskOpen(bool open) {
  if (open == oskOpen_)
    return;
  if (open && (dialogOpen_ || leaving_ || !live()))
    return;
  oskOpen_ = open;
  if (open) {
    // The pads drive the keyboard now: let go of whatever they hold in the game
    for (int i = 0; i < 4; i++)
      if (padConnected_[i])
        releasePad(i);
    releasePointerInputs();
    placeOsk();
    osk->setVisibility(brls::Visibility::VISIBLE);
  } else {
    osk->releaseAll();
    releasePointerInputs(); // ZL / ZR clicks
    osk->setVisibility(brls::Visibility::GONE);
  }
  // The button that opened / closed it isn't a key press for the other side
  suppressHeldPadButtons();
  for (bool &b : oskPadPrev_)
    b = false;
  oskShoulderChord_ = false;
  oskFocusDx_ = oskFocusDy_ = 0;
  oskFingers_.clear();
  otherFingers_.clear();
}

void StreamActivity::placeOsk() {
  if (oskDockTop_) {
    osk->setPositionBottom(brls::View::AUTO);
    osk->setPositionTop(0);
  } else {
    osk->setPositionTop(brls::View::AUTO);
    osk->setPositionBottom(0);
  }
  if (!oskButtonVisible_ || !oskOpen_)
    return;
  // Keep the floating button off the keyboard
  constexpr float kMargin = 24.0f;
  float rootH = root->getHeight() > 0 ? root->getHeight() : brls::Application::contentHeight;
  float kbH = osk->getHeight() > 0 ? osk->getHeight() : kOskHeight;
  float size = oskButtonExtent(oskButton->getHeight());
  float left = root->getX() + oskButtonLeft_;
  if (!oskDockTop_ && oskButtonTop_ + size > rootH - kbH)
    placeOskButton(left, root->getY() + rootH - kbH - size - kMargin);
  else if (oskDockTop_ && oskButtonTop_ < kbH)
    placeOskButton(left, root->getY() + kbH + kMargin);
}

vkpcnx::StreamOverlay::SessionMetrics StreamActivity::collectSessionMetrics() const {
  vkpcnx::StreamOverlay::SessionMetrics m{};
  m.gameServerName = gameServerName_;

  if (session_) {
    auto s = session_->stats();
    m.rtt = s.rtt ? static_cast<int>(s.rtt->count()) : static_cast<int>(s.inputRttMs);
    m.decodedFrames = static_cast<int>(s.decoder.framesDecoded);
    m.droppedFrames = static_cast<int>(s.decoder.framesDropped);
    m.decodeErrors = static_cast<int>(s.decoder.decodeErrors);
    m.avgDecodeMs = static_cast<int>(s.decoder.avgDecodeMs);
    m.frameWidth = s.decoder.width;
    m.frameHeight = s.decoder.height;
    m.videoCodecName = s.decoder.codecName;
    m.hwDecoderName = s.decoder.hwAccelName;
  }

  if (sessionStartedAt_)
    m.sessionSecondsElapsed =
      static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::steady_clock::now() - *sessionStartedAt_
      )
                         .count());

  m.timeLeftUnlimited = lastTimeLeftSeconds_ >= kUnlimitedTimeLeftThresholdSeconds;
  m.sessionSecondsLeft = m.timeLeftUnlimited ? 0 : static_cast<int>(lastTimeLeftSeconds_);

  return m;
}

void StreamActivity::showErrorAndLeave(const std::string &title, const std::string &message) {
  if (leaving_)
    return;
  leaving_ = true;
  setCaptured(false);
  overlay->setVisibility(brls::Visibility::GONE);
  auto *dialog = new brls::Dialog(title + "\n\n" + message);
  dialog->addButton("OK", [this] {
    session_->stop([] { brls::Application::popActivity(brls::TransitionAnimation::FADE); });
  });
  dialog->setCancelable(false);
  dialog->open();
}

void StreamActivity::leave() {
  if (leaving_)
    return;
  leaving_ = true;
  setCaptured(false);
  if (!session_) { // playground; may be called from draw(), so on the next loop
    brls::sync([] { brls::Application::popActivity(brls::TransitionAnimation::FADE); });
    return;
  }
  statusTitle->setText("Завершение");
  statusText->setText("Отключение от игрового сервера…");
  overlay->setVisibility(brls::Visibility::VISIBLE);
  session_->stop([] { brls::Application::popActivity(brls::TransitionAnimation::FADE); });
}
