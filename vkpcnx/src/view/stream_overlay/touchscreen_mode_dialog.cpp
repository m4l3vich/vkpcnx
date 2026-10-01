#include "view/stream_overlay/touchscreen_mode_dialog.hpp"
#include "borealis/core/view.hpp"
#include "core/settings.hpp"

using namespace brls::literals;

namespace vkpcnx {
TouchscreenModeDialog::TouchscreenModeDialog() {
  this->inflateFromXMLRes("xml/views/stream_overlay/touchscreen_mode_dialog.xml");

  this->trackpadRadio->registerClickAction([this](auto &&) {
    this->changeMode("trackpad");
    return true;
  });
  this->touchscreenRadio->registerClickAction([this](auto &&) {
    this->changeMode("touchscreen");
    return true;
  });

  this->touchscreenRadio->setCustomNavigationRoute(
    brls::FocusDirection::DOWN, "brls/dialog/button1"
  );
  renderCurrentMode();
}

void TouchscreenModeDialog::changeMode(std::string newMode) {
  Settings::instance().set("/controls/touchscreen_mode", newMode);
  this->renderCurrentMode();
}

void TouchscreenModeDialog::renderCurrentMode() {
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
} // namespace vkpcnx