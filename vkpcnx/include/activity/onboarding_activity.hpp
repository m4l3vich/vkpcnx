#pragma once
#include "borealis/core/activity.hpp"
#include "borealis/views/rectangle.hpp"
#include "view/fill_scrolling_frame.hpp"
#include "view/onboarding/controller_mapping.hpp"
#include "view/onboarding/done.hpp"
#include "view/onboarding/gamepad_pointer.hpp"
#include "view/onboarding/hotkeys.hpp"
#include "view/onboarding/start.hpp"
#include "view/onboarding/touchscreen_mode.hpp"
#include <borealis.hpp>
#include <ctime>

class OnboardingActivity : public brls::Activity, public vkpcnx::OnboardingNavigator {
  using Factory = std::function<vkpcnx::OnboardingScreen *()>;

  std::vector<Factory> screens = {
    [] { return new vkpcnx::Onboarding::Start; },
    [] { return new vkpcnx::Onboarding::TouchscreenMode; },
    [] { return new vkpcnx::Onboarding::ControllerMapping; },
    [] { return new vkpcnx::Onboarding::GamepadPointer; },
    [] { return new vkpcnx::Onboarding::Hotkeys; },
    [] { return new vkpcnx::Onboarding::Done; }
  };
  size_t index = 0;

  BRLS_BIND(brls::Box, progress, "onboarding/progress");
  BRLS_BIND(vkpcnx::FillScrollingFrame, content, "onboarding/content");
  BRLS_BIND(brls::Label, title, "onboarding/title");

public:
  CONTENT_FROM_XML_RES("activity/onboarding.xml");

  void onContentAvailable() override {
    for (size_t i = 0; i < screens.size(); i++) {
      auto *bar = new brls::Rectangle();
      bar->setGrow(1);
      bar->setHeight(5);
      bar->setCornerRadius(2.5f);
      if (i > 0)
        bar->setMarginLeft(5);
      progress->addView(bar);
    }
    // Synchronous: pushActivity asks the content for default focus right after this.
    mount(0);
  }

  void next() override {
    if (index + 1 < screens.size())
      show(index + 1);
    else
      finish();
  }

  void back() override {
    if (index > 0)
      show(index - 1);
    else
      brls::sync([] { brls::Application::popActivity(); });
  }

  void finish() override {
    brls::sync([] { brls::Application::popActivity(); });
  }

  void setTitle(std::string text) override {
    brls::sync([this, text] { title->setText(text); });
  }

private:
  // Screens live in a persistent host inside the scrolling frame, so the new
  // screen can be attached and focused before the old one is freed. Borealis
  // doesn't clear Application::currentFocus when the focused view is deleted.
  brls::Box *host = nullptr;
  vkpcnx::OnboardingScreen *current = nullptr;

  // Deferred: called from the current screen's handlers, which mount() deletes.
  void show(size_t i) {
    brls::sync([this, i] { mount(i); });
  }

  void mount(size_t i) {
    if (!host) {
      host = new brls::Box(brls::Axis::COLUMN);
      content->setContentView(host);
    }

    index = i;
    auto *screen = screens[i]();
    screen->setNavigator(this);
    screen->setGrow(1);
    host->addView(screen);

    if (current) {
      brls::Application::giveFocus(screen);
      host->removeView(current);
    }
    current = screen;
    updateProgress();
  }

  void updateProgress() {
    auto active = brls::Application::getTheme()["brls/accent"];
    auto idle = brls::Application::getTheme()["brls/header/border"];
    auto &bars = progress->getChildren();
    for (size_t i = 0; i < bars.size(); i++)
      static_cast<brls::Rectangle *>(bars[i])->setColor(i <= index ? active : idle);
  }
};