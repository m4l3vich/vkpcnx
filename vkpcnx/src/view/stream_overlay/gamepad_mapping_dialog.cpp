#include "view/stream_overlay/gamepad_mapping_dialog.hpp"
#include "borealis/core/view.hpp"
#include "core/settings.hpp"

using namespace brls::literals;

namespace vkpcnx {
GamepadMappingDialog::GamepadMappingDialog() {
  this->inflateFromXMLRes("xml/views/stream_overlay/gamepad_mapping_dialog.xml");

  this->literalRadio->registerClickAction([this](auto &&) {
    this->changeMode("literal");
    return true;
  });
  this->positionalRadio->registerClickAction([this](auto &&) {
    this->changeMode("positional");
    return true;
  });

  this->positionalRadio->setCustomNavigationRoute(
    brls::FocusDirection::DOWN, "brls/dialog/button1"
  );
  renderCurrentMode();
}

void GamepadMappingDialog::changeMode(std::string newMode) {
  Settings::instance().set("/controls/mapping_mode", newMode);
  this->renderCurrentMode();
}

void GamepadMappingDialog::renderCurrentMode() {
  auto mappingMode = Settings::instance().get<std::string>("/controls/mapping_mode", "positional");

  brls::View *literalViews[] = {&*this->literalDesc, &*this->literalPic};
  brls::View *positionalViews[] = {&*this->positionalDesc, &*this->positionalPic};

  auto literalVisibility = brls::Visibility::GONE;
  auto positionalVisibility = brls::Visibility::GONE;
  this->literalRadio->setSelected(false);
  this->positionalRadio->setSelected(false);

  if (mappingMode == "literal") {
    literalVisibility = brls::Visibility::VISIBLE;
    this->literalRadio->setSelected(true);
  } else if (mappingMode == "positional") {
    positionalVisibility = brls::Visibility::VISIBLE;
    this->positionalRadio->setSelected(true);
  }

  for (brls::View *view : literalViews) {
    view->setVisibility(literalVisibility);
  }
  for (brls::View *view : positionalViews) {
    view->setVisibility(positionalVisibility);
  }
}
} // namespace vkpcnx