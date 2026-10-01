#pragma once
#include "activity/stream_activity.hpp"
#include "core/settings.hpp"
#include "view/onboarding/onboarding_screen.hpp"
#include <borealis.hpp>

namespace vkpcnx::Onboarding {
class GamepadPointer : public OnboardingScreen {
private:
  void renderCurrentMode();

  BRLS_BIND(brls::DetailCell, pointerToggleCell, "onboarding/gamepad_pointer/pointer_toggle");
  BRLS_BIND(brls::Button, playgroundButton, "onboarding/gamepad_pointer/playground");

  BRLS_BIND(brls::Button, nextButton, "onboarding/gamepad_pointer/next");
  BRLS_BIND(brls::Button, backButton, "onboarding/gamepad_pointer/back");

public:
  GamepadPointer() : OnboardingScreen("xml/views/onboarding/gamepad_pointer.xml") {
    this->pointerToggleCell->registerClickAction([this](auto &&) {
      auto settings = Settings::instance();
      bool currentValue = settings.get<bool>("/controls/gamepad_pointer", false);
      Settings::instance().set("/controls/gamepad_pointer", !currentValue);
      this->renderCurrentMode();
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

    this->playgroundButton->registerClickAction([](brls::View *) {
      brls::Application::pushActivity(new StreamActivity(StreamActivity::Playground{}));
      return true;
    });

    setTitle("Контроллер как мышь");
  }

  ~GamepadPointer() { Settings::instance().save(); }
};
} // namespace vkpcnx::Onboarding
