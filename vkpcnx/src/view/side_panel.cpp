#include "view/side_panel.hpp"

using namespace brls::literals;

namespace vkpcnx {
SidePanel::SidePanel(brls::Box *contentView, float width) : panelWidth(width) {
  this->inflateFromXMLRes("xml/views/side_panel.xml");
  appletFrame->setWidth(panelWidth);
  container->addView(contentView);
  appletFrame->setFocusable(true);

  appletFrame->registerAction(
    "hints/back"_i18n,
    brls::BUTTON_B,
    [this](brls::View *view) {
      if (!cancelable || !backArmed)
        return false;
      this->dismiss();
      return true;
    },
    false,
    false,
    brls::SOUND_BACK
  );

  this->addGestureRecognizer(
    new brls::TapGestureRecognizer([this](brls::TapGestureStatus status, brls::Sound *) {
      if (status.state != brls::GestureState::END)
        return;

      if (cancelable && !appletFrame->getFrame().pointInside(status.position))
        this->dismiss();
    })
  );
}

SidePanel::~SidePanel() {
  if (haveArmSubscription)
    brls::Application::getRunLoopEvent()->unsubscribe(armSubscription);
}

brls::AppletFrame *SidePanel::getAppletFrame() { return appletFrame; }

brls::View *SidePanel::getDefaultFocus() {
  if (brls::View *focus = container->getDefaultFocus())
    return focus;
  return appletFrame;
}

void SidePanel::setCancelable(bool cancelable) { this->cancelable = cancelable; }

void SidePanel::open() {
  armBackWhenReleased();
  brls::Application::pushActivity(new brls::Activity(this));
}

void SidePanel::armBackWhenReleased() {
  auto isBackHeld = [] {
    brls::ControllerState state{};
    brls::Application::getPlatform()->getInputManager()->updateUnifiedControllerState(&state);
    return state.buttons[brls::BUTTON_B];
  };

  backArmed = !isBackHeld();
  if (backArmed || haveArmSubscription)
    return;

  // Unsubscribing from inside the callback would invalidate the iterator
  // Event::fire() is walking, so the subscription lives until the destructor.
  haveArmSubscription = true;
  armSubscription = brls::Application::getRunLoopEvent()->subscribe([this, isBackHeld] {
    if (!backArmed && !isBackHeld())
      backArmed = true;
  });
}

void SidePanel::show(std::function<void(void)> cb, bool animate, float animationDuration) {
  if (animate) {
    appletFrame->setTranslationX(panelWidth);

    slideOffset.stop();
    slideOffset.reset(panelWidth);
    slideOffset.addStep(0, animationDuration, brls::EasingFunction::quadraticOut);
    slideOffset.setTickCallback([this] { this->slideTick(); });
    slideOffset.start();
  }

  Box::show(cb, animate, animationDuration);
}

void SidePanel::hide(std::function<void(void)> cb, bool animated, float animationDuration) {
  if (animated) {
    slideOffset.stop();
    slideOffset.reset(0);
    slideOffset.addStep(panelWidth, animationDuration, brls::EasingFunction::quadraticIn);
    slideOffset.setTickCallback([this] { this->slideTick(); });
    slideOffset.start();
  }

  Box::hide(cb, animated, animationDuration);
}

void SidePanel::slideTick() { appletFrame->setTranslationX(slideOffset); }
} // namespace vkpcnx