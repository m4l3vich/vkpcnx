#include "view/stream_overlay/gamepad_pointer_dialog.hpp"
#include "borealis/core/view.hpp"
#include "core/settings.hpp"

using namespace brls::literals;

namespace vkpcnx {
void GamepadPointerDialog::bindSensitivity(
  brls::SliderCell *cell, const vkpcnx::controls::Sensitivity &s
) {
  float value = controls::get(s);
  cell->setDetailText(controls::format(value));
  cell->slider->setProgress(controls::toProgress(s, value));
  cell->getEvent()->subscribe([cell, &s](float progress) {
    float v = controls::fromProgress(s, progress);
    controls::set(s, v, false);
    cell->setDetailText(controls::format(v));
  });
}

GamepadPointerDialog::GamepadPointerDialog() {
  this->inflateFromXMLRes("xml/views/stream_overlay/gamepad_pointer_dialog.xml");

  this->pointerToggleCell->registerClickAction([this](auto &&) {
    auto settings = Settings::instance();
    bool currentValue = settings.get<bool>("/controls/gamepad_pointer", false);
    Settings::instance().set("/controls/gamepad_pointer", !currentValue);
    this->renderCurrentMode();
    return true;
  });

  bindSensitivity(cursorSpeedCell, controls::kGamepadCursor);
  bindSensitivity(scrollSpeedCell, controls::kGamepadScroll);

  this->pointerToggleCell->setCustomNavigationRoute(
    brls::FocusDirection::DOWN, "brls/dialog/button1"
  );
  renderCurrentMode();
}

void GamepadPointerDialog::renderCurrentMode() {
  bool currentValue = Settings::instance().get<bool>("/controls/gamepad_pointer", false);

  NVGcolor detailColor = brls::Application::getTheme().getColor(
    currentValue ? "brls/slider/line_filled" : "brls/slider/line_empty"
  );

  this->pointerToggleCell->setDetailText(currentValue ? "Вкл" : "Выкл");
  this->pointerToggleCell->setDetailTextColor(detailColor);
}
} // namespace vkpcnx