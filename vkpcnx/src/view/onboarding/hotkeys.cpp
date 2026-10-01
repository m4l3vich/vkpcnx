#include "view/onboarding/hotkeys.hpp"

namespace vkpcnx::Onboarding {
// Same trigger as StreamActivity::pollGamepads(): L+R held, on the press of
// the third button. All pads (and the desktop keyboard mapping) act as one.
void Hotkeys::pollHotkeys() {
  brls::ControllerState state{};
  brls::Application::getPlatform()->getInputManager()->updateUnifiedControllerState(&state);

  bool changed = false;
  if (state.buttons[brls::BUTTON_LB] && state.buttons[brls::BUTTON_RB]) {
    for (auto &hotkey : this->hotkeys) {
      if (!hotkey.done && state.buttons[hotkey.button] && !this->prevButtons[hotkey.button]) {
        hotkey.done = true;
        this->renderCheck(hotkey);
        changed = true;
      }
    }
  }

  for (int b = 0; b < brls::_BUTTON_MAX; b++)
    this->prevButtons[b] = state.buttons[b];

  if (changed && this->allDone())
    this->nextButton->setState(brls::ButtonState::ENABLED);
}

bool Hotkeys::shouldersHeld() const {
  brls::ControllerState state{};
  brls::Application::getPlatform()->getInputManager()->updateUnifiedControllerState(&state);
  return state.buttons[brls::BUTTON_LB] && state.buttons[brls::BUTTON_RB];
}

void Hotkeys::renderCheck(const Hotkey &hotkey) {
  hotkey.check->setText(hotkey.done ? "" : ""); // check_box / check_box_outline_blank
  hotkey.check->setTextColor(
    brls::Application::getTheme().getColor(hotkey.done ? "brls/accent" : "brls/text_disabled")
  );
}

bool Hotkeys::allDone() const {
  for (const auto &hotkey : this->hotkeys)
    if (!hotkey.done)
      return false;
  return true;
}
} // namespace vkpcnx::Onboarding
