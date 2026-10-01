#pragma once
#include "view/animated_webp.hpp"
#include "view/onboarding/onboarding_screen.hpp"
#include <borealis.hpp>

namespace vkpcnx::Onboarding {
class TouchscreenMode : public OnboardingScreen {
private:
  void changeMode(std::string newMode);
  void renderCurrentMode();

  BRLS_BIND(vkpcnx::AnimatedWebp, anim, "onboarding/touchscreen_mode/anim");

  BRLS_BIND(brls::RadioCell, trackpadRadio, "onboarding/touchscreen_mode/trackpad_radio");
  BRLS_BIND(brls::RadioCell, touchscreenRadio, "onboarding/touchscreen_mode/touchscreen_radio");

  BRLS_BIND(brls::Label, trackpadDesc, "onboarding/touchscreen_mode/trackpad_desc");
  BRLS_BIND(brls::Label, touchscreenDesc, "onboarding/touchscreen_mode/touchscreen_desc");

  BRLS_BIND(brls::Button, nextButton, "onboarding/touchscreen_mode/next");
  BRLS_BIND(brls::Button, backButton, "onboarding/touchscreen_mode/back");

public:
  TouchscreenMode() : OnboardingScreen("xml/views/onboarding/touchscreen_mode.xml") {
    this->trackpadRadio->registerClickAction([this](auto &&) {
      this->changeMode("trackpad");
      return true;
    });
    this->touchscreenRadio->registerClickAction([this](auto &&) {
      this->changeMode("touchscreen");
      return true;
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

    setTitle("Режим сенсорного экрана");
  }
};
} // namespace vkpcnx::Onboarding
