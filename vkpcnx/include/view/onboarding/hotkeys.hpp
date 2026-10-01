#pragma once
#include "borealis/views/button.hpp"
#include "view/onboarding/onboarding_screen.hpp"
#include <borealis.hpp>

namespace vkpcnx::Onboarding {
// Continue stays disabled until each of the stream's client-side hotkeys
// (activity/stream_activity.hpp: L+R+Y, L+R+Minus, L+R+Plus) has been pressed.
class Hotkeys : public OnboardingScreen {
  struct Hotkey {
    brls::ControllerButton button;
    brls::Label *check;
    bool done;
  };

  void pollHotkeys();
  bool shouldersHeld() const;
  void renderCheck(const Hotkey &hotkey);
  bool allDone() const;

  BRLS_BIND(brls::Label, overlayCheck, "onboarding/hotkeys/overlay_check");
  BRLS_BIND(brls::Label, keyboardCheck, "onboarding/hotkeys/keyboard_check");
  BRLS_BIND(brls::Label, pointerCheck, "onboarding/hotkeys/pointer_check");

  BRLS_BIND(brls::Button, nextButton, "onboarding/hotkeys/next");
  BRLS_BIND(brls::Button, backButton, "onboarding/hotkeys/back");

  Hotkey hotkeys[3] = {
    {brls::BUTTON_Y, overlayCheck, false},
    {brls::BUTTON_BACK, keyboardCheck, false}, // Minus on Switch
    {brls::BUTTON_START, pointerCheck, false}, // Plus on Switch
  };
  bool prevButtons[brls::_BUTTON_MAX] = {};
  brls::VoidEvent::Subscription loopSub;

public:
  Hotkeys() : OnboardingScreen("xml/views/onboarding/hotkeys.xml") {
#ifndef __SWITCH__
    this->nextButton->setState(brls::ButtonState::ENABLED);
    this->nextButton->setText("Пропустить");
#endif

    this->nextButton->registerClickAction([this](brls::View *) {
#ifdef __SWITCH__
      if (this->allDone())
#endif
        this->next();
      return true;
    });
    this->backButton->registerClickAction([this](brls::View *) {
      this->back();
      return true;
    });

    // The chords are detected in pollHotkeys(); their last button must not
    // reach the activity (Plus is the global Exit). Alone it falls through.
    for (const auto &hotkey : this->hotkeys) {
      this->registerAction(
        "", hotkey.button, [this](brls::View *) { return this->shouldersHeld(); }, true
      );
    }

    // Chords already held when the screen opens don't count
    brls::ControllerState state{};
    brls::Application::getPlatform()->getInputManager()->updateUnifiedControllerState(&state);
    for (int b = 0; b < brls::_BUTTON_MAX; b++)
      this->prevButtons[b] = state.buttons[b];

    this->loopSub =
      brls::Application::getRunLoopEvent()->subscribe([this] { this->pollHotkeys(); });

    setTitle("Сочетания кнопок");
  }

  ~Hotkeys() { brls::Application::getRunLoopEvent()->unsubscribe(this->loopSub); }
};
} // namespace vkpcnx::Onboarding
