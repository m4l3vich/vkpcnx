#pragma once

#include <borealis.hpp>

namespace vkpcnx {
class SidePanel : public brls::Box {
public:
  SidePanel(brls::Box *contentView, float width = 480);
  ~SidePanel() override;

  brls::AppletFrame *getAppletFrame() override;
  brls::View *getDefaultFocus() override;

  void setCancelable(bool cancelable);

  void open();

  void show(std::function<void(void)> cb, bool animate, float animationDuration) override;
  void hide(std::function<void(void)> cb, bool animated, float animationDuration) override;

  bool isTranslucent() override { return true; }

private:
  BRLS_BIND(brls::AppletFrame, appletFrame, "sidepanel/applet");
  BRLS_BIND(brls::Box, container, "sidepanel/container");

  float panelWidth;
  bool cancelable = true;
  brls::Animatable slideOffset = 0;

  // The panel may be summoned by holding a key that borealis also maps to
  // BUTTON_B (e.g. Esc). Input dispatch is blocked while the stream is
  // captured, so the first frame after we open sees that key as a fresh press
  // and would dismiss us right away. Ignore "back" until B has been released.
  bool backArmed = true;
  brls::VoidEvent::Subscription armSubscription{};
  bool haveArmSubscription = false;

  void armBackWhenReleased();
  void slideTick();
};
}; // namespace vkpcnx