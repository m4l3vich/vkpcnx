#include "view/onboarding/controller_mapping.hpp"
#include "borealis/core/view.hpp"
#include "core/settings.hpp"

namespace vkpcnx::Onboarding {
void ControllerMapping::changeMode(std::string newMode) {
  Settings::instance().set("/controls/mapping_mode", newMode);
  swapPending = true;
  fadeTo(0.0f);
}

void ControllerMapping::fadeTo(float target) {
  int32_t half = brls::Application::getStyle()["brls/animations/show"] * 2;
  fade.reset(fade.getValue());
  fade.addStep(
    target,
    half,
    target == 0.0f ? brls::EasingFunction::quadraticIn : brls::EasingFunction::quadraticOut
  );
  fade.start();
}

void ControllerMapping::renderCurrentMode() {
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
} // namespace vkpcnx::Onboarding