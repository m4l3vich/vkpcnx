#pragma once
#include "borealis/views/button.hpp"
#include "view/onboarding/onboarding_screen.hpp"
#include "view/onboarding/skip_dialog.hpp"
#include <borealis.hpp>

namespace vkpcnx::Onboarding {
class Start : public OnboardingScreen {
  BRLS_BIND(brls::Button, nextButton, "onboarding/start/proceed");
  BRLS_BIND(brls::Button, skipButton, "onboarding/start/skip");

public:
  Start() : OnboardingScreen("xml/views/onboarding/start.xml") {
    nextButton->registerClickAction([this](brls::View *) {
      this->next();
      return true;
    });
    skipButton->registerClickAction([this](brls::View *) {
      auto *dialog = new brls::Dialog(new SkipDialog());
      dialog->addButton("Назад", [] {});
      dialog->addButton("Да, понятно", [this] { this->finish(); });
      dialog->setCancelable(true);
      dialog->open();
      return true;
    });

    setTitle("Первоначальная настройка");
  }
};
} // namespace vkpcnx::Onboarding