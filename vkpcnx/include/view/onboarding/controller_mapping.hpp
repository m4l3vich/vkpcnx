#pragma once
#include "view/onboarding/onboarding_screen.hpp"
#include <borealis.hpp>

namespace vkpcnx::Onboarding {
class ControllerMapping : public OnboardingScreen {
private:
  void changeMode(std::string newMode);
  void renderCurrentMode();
  void fadeTo(float target);

  brls::Animatable fade = 1.0f;
  bool swapPending = false;

  BRLS_BIND(brls::Image, literalPic, "onboarding/gamepad_mapping/literal_pic");
  BRLS_BIND(brls::Image, positionalPic, "onboarding/gamepad_mapping/positional_pic");

  BRLS_BIND(brls::Label, literalDesc, "onboarding/gamepad_mapping/literal_desc");
  BRLS_BIND(brls::Label, positionalDesc, "onboarding/gamepad_mapping/positional_desc");

  BRLS_BIND(brls::RadioCell, literalRadio, "onboarding/gamepad_mapping/literal_radio");
  BRLS_BIND(brls::RadioCell, positionalRadio, "onboarding/gamepad_mapping/positional_radio");

  BRLS_BIND(brls::Button, nextButton, "onboarding/gamepad_mapping/next");
  BRLS_BIND(brls::Button, backButton, "onboarding/gamepad_mapping/back");

public:
  ControllerMapping() : OnboardingScreen("xml/views/onboarding/controller_mapping.xml") {
    this->literalRadio->registerClickAction([this](auto &&) {
      this->changeMode("literal");
      return true;
    });
    this->positionalRadio->registerClickAction([this](auto &&) {
      this->changeMode("positional");
      return true;
    });

    fade.setTickCallback([this] {
      brls::View *views[] = {&*literalPic, &*positionalPic};
      for (auto *view : views)
        view->setAlpha(fade);
    });
    fade.setEndCallback([this](bool finished) {
      if (!finished || !swapPending)
        return;
      swapPending = false;
      renderCurrentMode();
      fadeTo(1.0f);
    });

    renderCurrentMode();

    this->nextButton->registerClickAction([this](brls::View *) {
      this->next();
      return true;
    });
    this->backButton->registerClickAction([this](brls::View *) {
      this->back();
      return true;
    });

    setTitle("Раскладка контроллера");
  }
};
} // namespace vkpcnx::Onboarding
