#include "view/stream_overlay.hpp"
#include "borealis/core/view.hpp"
#include "core/settings.hpp"
#include <borealis.hpp>
#include <ctime>
#include <fmt/std.h>
#include <string>

namespace vkpcnx {
std::string StreamOverlay::formatSeconds(int value) {
  if (value == 0)
    return "-";

  int hrs = value / (60 * 60);
  int mins = (value / 60) % 60;
  int secs = value % 60;

  if (hrs > 0) {
    return fmt::format("{}ч {}м", hrs, mins);
  } else {
    return fmt::format("{}м {}с", mins, secs);
  }
};

namespace {
template <typename Setter>
void setIfChanged(std::string &cache, std::string value, Setter &&setter) {
  if (cache == value)
    return;
  cache = value;
  setter(cache);
}
} // namespace

void StreamOverlay::renderInfo() {
  if (metricsProvider)
    metrics = metricsProvider();

  // Clock
  char time[5];
  std::time_t t = std::time(nullptr);
  std::strftime(time, 5, "%H:%M", std::localtime(&t));
  setIfChanged(this->lastClock, time, [this](auto &v) { this->clockLabel->setText(v); });

  // Battery
  setIfChanged(
    this->lastBatteryPct,
    std::to_string(brls::Application::getPlatform()->getBatteryLevel()),
    [this](auto &v) { this->batteryPctLabel->setText(v); }
  );

  // Stats visibility (hidden for good in the playground)
  if (!this->playground) {
    this->statsShortBox->setVisibility(
      !this->showAllMetrics ? brls::Visibility::VISIBLE : brls::Visibility::GONE
    );
    this->statsFullBox->setVisibility(
      this->showAllMetrics ? brls::Visibility::VISIBLE : brls::Visibility::GONE
    );
  }

  // Short stats: ping
  setIfChanged(this->lastPing, std::to_string(metrics.rtt) + "мс", [this](auto &v) {
    this->pingLabel->setText(v);
  });

  // Short stats: time left
  std::string timeLeftStr =
    metrics.timeLeftUnlimited ? "Неогранич." : formatSeconds(metrics.sessionSecondsLeft);

  setIfChanged(this->lastServerName, metrics.gameServerName, [this](auto &v) {
    this->serverNameLabel->setText(v);
  });

  // Full stats
  float droppedFramesPct = ((float)metrics.droppedFrames / (float)metrics.decodedFrames) * 100;

  setIfChanged(this->lastStatsFullPing, std::to_string(metrics.rtt) + "мс", [this](auto &v) {
    this->statsFullPing->setValue(v);
  });
  setIfChanged(this->lastStatsFullServerName, metrics.gameServerName, [this](auto &v) {
    this->statsFullServerName->setValue(v);
  });
  setIfChanged(
    this->lastSessionTimeElapsed, formatSeconds(metrics.sessionSecondsElapsed), [this](auto &v) {
      this->sessionTimeElapsed->setValue(v);
    }
  );
  setIfChanged(this->lastStatsFullTimeLeft, timeLeftStr, [this](auto &v) {
    this->statsFullTimeLeft->setValue(v);
  });
  setIfChanged(
    this->lastDroppedFrames,
    fmt::format("{} ({:.2f}%)", metrics.droppedFrames, droppedFramesPct),
    [this](auto &v) { this->droppedFrames->setValue(v); }
  );
  setIfChanged(this->lastDecodeErrs, std::to_string(metrics.decodeErrors), [this](auto &v) {
    this->decodeErrs->setValue(v);
  });
  setIfChanged(this->lastAvgDecodeMs, std::to_string(metrics.avgDecodeMs) + "мс", [this](auto &v) {
    this->avgDecodeMs->setValue(v);
  });
  setIfChanged(
    this->lastStreamRes,
    fmt::format("{}\u00d7{}", metrics.frameWidth, metrics.frameHeight),
    [this](auto &v) { this->streamRes->setValue(v); }
  );

  setIfChanged(this->lastVideoCodec, metrics.videoCodecName, [this](auto &v) {
    this->videoCodec->setValue(v);
  });

  std::string hwDecoderName = "Нет";
  if (metrics.hwDecoderName) {
    hwDecoderName = fmt::format("Да ({})", *metrics.hwDecoderName);
  }
  setIfChanged(this->lastHwAccel, hwDecoderName, [this](auto &v) { this->hwAccel->setValue(v); });

  // Controls
  auto settings = Settings::instance();

  // Controls: mapping mode
  auto mappingMode = settings.get<std::string>("/controls/mapping_mode", "positional");
  std::string mappingModeName = std::unordered_map<std::string, std::string>{
    {"literal", "Буквальная"}, {"positional", "Позиционная"}
  }[mappingMode];

  setIfChanged(this->lastMappingModeName, mappingModeName, [this](auto &v) {
    this->gamepadMappingModeCell->setDetailText(v);
  });

  // Controls: gamepad pointer mode
  auto gamepadPointerEnabled = settings.get<bool>("/controls/gamepad_pointer", false);
  NVGcolor pointerDetailColor = brls::Application::getTheme().getColor(
    gamepadPointerEnabled ? "brls/slider/line_filled" : "brls/slider/line_empty"
  );

  setIfChanged(
    this->lastGamepadPointerText, gamepadPointerEnabled ? "Вкл" : "Выкл", [this](auto &v) {
      this->gamepadPointerCell->setDetailText(v);
    }
  );
  this->gamepadPointerCell->setDetailTextColor(pointerDetailColor);

  // Controls: touchscreen mode
  auto touchscreenMode = settings.get<std::string>("/controls/touchscreen_mode", "touchscreen");
  std::string touchscreenModeName = std::unordered_map<std::string, std::string>{
    {"trackpad", "Трекпад"}, {"touchscreen", "Тачскрин"}
  }[touchscreenMode];

  setIfChanged(this->lastTouchscreenModeText, touchscreenModeName, [this](auto &v) {
    this->touchscreenModeCell->setDetailText(v);
  });
}

void StreamOverlay::openDialog(DialogContent *content) {
  brls::Dialog *dialog = new brls::Dialog(content);
  dialog->addButton("Сохранить", [] {});
  dialog->setCancelable(true);
  dialog->getAppletFrame()->setWidth(800);
  dialog->getAppletFrame()->setHeightPercentage(90.0f);
  dialog->setLastFocusedView(content->getScrollingFrame());
  dialog->open();
}
} // namespace vkpcnx
