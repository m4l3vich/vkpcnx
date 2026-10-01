#pragma once
#include "view/onboarding/onboarding_screen.hpp"
#include <borealis.hpp>

namespace vkpcnx::Onboarding {
class Done : public OnboardingScreen {
  BRLS_BIND(brls::Button, finishButton, "onboarding/done");

public:
  Done() : OnboardingScreen("xml/views/onboarding/done.xml") {
    this->finishButton->registerClickAction([this](brls::View *) {
      this->next();
      return true;
    });
    setTitle("Настройка завершена");
  }
};
} // namespace vkpcnx::Onboarding
