#pragma once
#include "borealis/views/cells/cell_radio.hpp"
#include "borealis/views/scrolling_frame.hpp"
#include "view/animated_webp.hpp"
#include "view/stream_overlay/dialog_content.hpp"
#include <borealis.hpp>

namespace vkpcnx {
class TouchscreenModeDialog : public DialogContent {
public:
  TouchscreenModeDialog();
  BRLS_BIND(brls::ScrollingFrame, scrollingFrame, "touchscreen_mode_dialog/scrolling_frame");

  brls::ScrollingFrame *getScrollingFrame() override { return this->scrollingFrame; }
  brls::Box *getContentBox() override { return this->content; }

private:
  void changeMode(std::string newMode);
  void renderCurrentMode();

  BRLS_BIND(brls::Box, content, "touchscreen_mode_dialog/content");
  BRLS_BIND(vkpcnx::AnimatedWebp, anim, "touchscreen_mode_dialog/anim");

  BRLS_BIND(brls::Label, trackpadDesc, "touchscreen_mode_dialog/trackpad_desc");
  BRLS_BIND(brls::Label, touchscreenDesc, "touchscreen_mode_dialog/touchscreen_desc");

  BRLS_BIND(brls::RadioCell, trackpadRadio, "touchscreen_mode_dialog/trackpad_radio");
  BRLS_BIND(brls::RadioCell, touchscreenRadio, "touchscreen_mode_dialog/touchscreen_radio");
};
}; // namespace vkpcnx