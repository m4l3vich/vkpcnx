#include "view/onboarding/gamepad_pointer.hpp"
#include "core/controls.hpp"

namespace vkpcnx::Onboarding {
void GamepadPointer::renderCurrentMode() {
  bool currentValue = Settings::instance().get<bool>("/controls/gamepad_pointer", false);

  NVGcolor detailColor = brls::Application::getTheme().getColor(
    currentValue ? "brls/slider/line_filled" : "brls/slider/line_empty"
  );

  this->pointerToggleCell->setDetailText(currentValue ? "Вкл" : "Выкл");
  this->pointerToggleCell->setDetailTextColor(detailColor);
}
} // namespace vkpcnx::Onboarding