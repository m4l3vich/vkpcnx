#include "view/onboarding/touchscreen_mode.hpp"
#include "borealis/core/view.hpp"
#include "core/settings.hpp"

namespace vkpcnx::Onboarding {
void TouchscreenMode::changeMode(std::string newMode) {
  Settings::instance().set("/controls/touchscreen_mode", newMode);
  this->renderCurrentMode();
}

void TouchscreenMode::renderCurrentMode() {
  auto mappingMode =
    Settings::instance().get<std::string>("/controls/touchscreen_mode", "touchscreen");

  this->trackpadDesc->setVisibility(brls::Visibility::GONE);
  this->touchscreenDesc->setVisibility(brls::Visibility::GONE);
  this->trackpadRadio->setSelected(false);
  this->touchscreenRadio->setSelected(false);

  if (mappingMode == "trackpad") {
    this->trackpadDesc->setVisibility(brls::Visibility::VISIBLE);
    this->trackpadRadio->setSelected(true);
    this->anim->setWebpFromRes("img/trackpad_anim.webp");
  } else if (mappingMode == "touchscreen") {
    this->touchscreenDesc->setVisibility(brls::Visibility::VISIBLE);
    this->touchscreenRadio->setSelected(true);
    this->anim->setWebpFromRes("img/touchscreen_anim.webp");
  }
}
} // namespace vkpcnx::Onboarding