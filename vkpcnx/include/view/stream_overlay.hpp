#pragma once
#include "borealis/views/cells/cell_detail.hpp"
#include "view/stream_overlay/dialog_content.hpp"
#include "view/stream_overlay/gamepad_mapping_dialog.hpp"
#include "view/stream_overlay/gamepad_pointer_dialog.hpp"
#include "view/stream_overlay/touchscreen_mode_dialog.hpp"
#include "view/striped_list.hpp"
#include <borealis.hpp>
#include <nlohmann/json.hpp>

namespace vkpcnx {
class StreamOverlay : public brls::Box {
public:
  StreamOverlay() {
    this->inflateFromXMLRes("xml/views/stream_overlay.xml");
    this->loopSubscription =
      brls::Application::getRunLoopEvent()->subscribe([this] { this->renderInfo(); });

    this->statsShortBox->getFocusEvent()->subscribe([this](auto &&) {
      this->showMoreLabel->setText("\uE0E0 Показать больше");
    });
    this->statsShortBox->getFocusLostEvent()->subscribe([this](auto &&) {
      this->showMoreLabel->setText("Показать больше");
    });

    this->statsFullBox->getFocusEvent()->subscribe([this](auto &&) {
      this->showLessLabel->setText("\uE0E0 Показать меньше");
    });
    this->statsFullBox->getFocusLostEvent()->subscribe([this](auto &&) {
      this->showLessLabel->setText("Показать меньше");
    });

    this->statsShortBox->registerClickAction([this](auto &&) {
      this->showAllMetrics = true;
      this->statsShortBox->setVisibility(brls::Visibility::GONE);
      this->statsFullBox->setVisibility(brls::Visibility::VISIBLE);
      brls::Application::giveFocus(this->statsFullBox);
      return true;
    });
    this->statsFullBox->registerClickAction([this](auto &&) {
      this->showAllMetrics = false;
      this->statsFullBox->setVisibility(brls::Visibility::GONE);
      this->statsShortBox->setVisibility(brls::Visibility::VISIBLE);
      brls::Application::giveFocus(this->statsShortBox);
      return true;
    });

    this->gamepadMappingModeCell->registerClickAction([this](auto &&) {
      this->openDialog(new GamepadMappingDialog());
      return true;
    });
    this->gamepadPointerCell->registerClickAction([this](auto &&) {
      this->openDialog(new GamepadPointerDialog());
      return true;
    });
    this->touchscreenModeCell->registerClickAction([this](auto &&) {
      this->openDialog(new TouchscreenModeDialog());
      return true;
    });

    this->showOnscreenKeyboardBtn->registerClickAction([this](auto &&) {
      if (this->onOpenOnscreenKeyboard)
        this->onOpenOnscreenKeyboard();
      return true;
    });
    this->onscreenKeyboardCell->registerClickAction([this](auto &&) {
      if (this->onToggleOskButton)
        this->onToggleOskButton();
      return true;
    });
    this->toggleDebugOverlayBtn->registerClickAction([this](auto &&) {
      if (this->onToggleDebugOverlay)
        this->onToggleDebugOverlay();
      return true;
    });
    this->endSessionBtn->registerClickAction([this](auto &&) {
      if (this->onEndSession)
        this->onEndSession();
      return true;
    });
  }
  ~StreamOverlay() { brls::Application::getRunLoopEvent()->unsubscribe(this->loopSubscription); }

  struct SessionMetrics {
    std::string gameServerName;
    int rtt;
    bool timeLeftUnlimited;
    int sessionSecondsLeft;
    int sessionSecondsElapsed;
    int decodedFrames;
    int droppedFrames;
    int decodeErrors;
    int avgDecodeMs;
    int frameWidth;
    int frameHeight;
    std::string videoCodecName;
    std::optional<std::string> hwDecoderName;
  };

  void setSessionMetrics(SessionMetrics metrics) { this->metrics = metrics; };

  void setMetricsProvider(std::function<SessionMetrics()> provider) {
    this->metricsProvider = std::move(provider);
  }

  void setDebugOverlayVisible(bool visible) {
    this->toggleDebugOverlayBtn->setText(
      visible ? "Скрыть дебаг-оверлей" : "Показать дебаг-оверлей"
    );
  }

  // On-screen keyboard button (session-only, not a Settings key)
  void setOskButtonVisible(bool visible) {
    this->onscreenKeyboardCell->setDetailText(visible ? "Вкл" : "Выкл");
    this->onscreenKeyboardCell->setDetailTextColor(
      brls::Application::getTheme().getColor(
        visible ? "brls/slider/line_filled" : "brls/slider/line_empty"
      )
    );
  }

  // Controls playground: no session statistics or debug overlay, and the
  // end-session button leaves the playground
  void setPlaygroundMode() {
    this->playground = true;
    this->statsHeader->setVisibility(brls::Visibility::GONE);
    this->statsShortBox->setVisibility(brls::Visibility::GONE);
    this->statsFullBox->setVisibility(brls::Visibility::GONE);
    this->toggleDebugOverlayBtn->setVisibility(brls::Visibility::GONE);
    this->endSessionBtn->setText("Выйти из песочницы");
  }

  std::function<void()> onOpenOnscreenKeyboard; // closes the overlay first
  std::function<void()> onToggleOskButton;
  std::function<void()> onToggleDebugOverlay;
  std::function<void()> onEndSession;

private:
  BRLS_BIND(brls::Box, root, "stream_overlay/root");

  BRLS_BIND(brls::Label, clockLabel, "stream_overlay/clock");
  BRLS_BIND(brls::Label, batteryPctLabel, "stream_overlay/battery_pct");

  BRLS_BIND(brls::Label, statsHeader, "stream_overlay/stats_header");
  BRLS_BIND(brls::Box, statsShortBox, "stream_overlay/stats_short");
  BRLS_BIND(brls::Label, pingLabel, "stream_overlay/ping");
  BRLS_BIND(brls::Label, serverNameLabel, "stream_overlay/server_name");
  BRLS_BIND(brls::Label, showMoreLabel, "stream_overlay/show_more");

  BRLS_BIND(brls::Button, showOnscreenKeyboardBtn, "stream_overlay/show_onscreen_keyboard");
  BRLS_BIND(brls::DetailCell, onscreenKeyboardCell, "stream_overlay/onscreen_keyboard");

  BRLS_BIND(brls::Box, statsFullBox, "stream_overlay/stats_full");
  BRLS_BIND(vkpcnx::StripedListRow, statsFullPing, "stream_overlay/stats_full/ping");
  BRLS_BIND(vkpcnx::StripedListRow, statsFullServerName, "stream_overlay/stats_full/server_name");
  BRLS_BIND(
    vkpcnx::StripedListRow, sessionTimeElapsed, "stream_overlay/stats_full/session_time_elapsed"
  );
  BRLS_BIND(
    vkpcnx::StripedListRow, statsFullTimeLeft, "stream_overlay/stats_full/session_time_left"
  );
  BRLS_BIND(vkpcnx::StripedListRow, droppedFrames, "stream_overlay/stats_full/dropped_frames");
  BRLS_BIND(vkpcnx::StripedListRow, decodeErrs, "stream_overlay/stats_full/decode_errs");
  BRLS_BIND(vkpcnx::StripedListRow, avgDecodeMs, "stream_overlay/stats_full/avg_decode_ms");
  BRLS_BIND(vkpcnx::StripedListRow, streamRes, "stream_overlay/stats_full/stream_res");
  BRLS_BIND(vkpcnx::StripedListRow, videoCodec, "stream_overlay/stats_full/video_codec");
  BRLS_BIND(vkpcnx::StripedListRow, hwAccel, "stream_overlay/stats_full/hw_accel");
  BRLS_BIND(brls::Label, showLessLabel, "stream_overlay/show_less");

  BRLS_BIND(brls::DetailCell, gamepadMappingModeCell, "stream_overlay/gamepad_mapping_mode");
  BRLS_BIND(brls::DetailCell, gamepadPointerCell, "stream_overlay/gamepad_pointer");
  BRLS_BIND(brls::DetailCell, touchscreenModeCell, "stream_overlay/touchscreen_mode");

  BRLS_BIND(brls::Button, toggleDebugOverlayBtn, "stream_overlay/toggle_debug_overlay");
  BRLS_BIND(brls::Button, endSessionBtn, "stream_overlay/end_session");

  std::string formatSeconds(int value);
  void renderInfo();
  void openDialog(DialogContent *content);

  brls::VoidEvent::Subscription loopSubscription;
  SessionMetrics metrics{};
  std::function<SessionMetrics()> metricsProvider;
  bool showAllMetrics = false;
  bool playground = false;

  std::string lastClock;
  std::string lastBatteryPct;
  std::string lastPing;
  std::string lastServerName;
  std::string lastStatsFullPing;
  std::string lastStatsFullServerName;
  std::string lastSessionTimeElapsed;
  std::string lastStatsFullTimeLeft;
  std::string lastDroppedFrames;
  std::string lastDecodeErrs;
  std::string lastAvgDecodeMs;
  std::string lastStreamRes;
  std::string lastVideoCodec;
  std::string lastHwAccel;
  std::string lastMappingModeName;
  std::string lastGamepadPointerText;
  std::string lastTouchscreenModeText;
};
}; // namespace vkpcnx
