#pragma once
#include "core/controls.hpp"
#include "view/stream_overlay/dialog_content.hpp"
#include <borealis.hpp>

namespace vkpcnx {
class GamepadPointerDialog : public DialogContent {
public:
  GamepadPointerDialog();
  BRLS_BIND(brls::ScrollingFrame, scrollingFrame, "gamepad_pointer_dialog/scrolling_frame");

  brls::ScrollingFrame *getScrollingFrame() override { return this->scrollingFrame; }
  brls::Box *getContentBox() override { return this->content; }

private:
  void renderCurrentMode();
  static void bindSensitivity(brls::SliderCell *cell, const vkpcnx::controls::Sensitivity &s);

  BRLS_BIND(brls::Box, content, "gamepad_pointer_dialog/content");
  BRLS_BIND(brls::DetailCell, pointerToggleCell, "gamepad_pointer_dialog/pointer_toggle");
  BRLS_BIND(brls::SliderCell, cursorSpeedCell, "gamepad_pointer_dialog/cursor_speed");
  BRLS_BIND(brls::SliderCell, scrollSpeedCell, "gamepad_pointer_dialog/scroll_speed");
};
}; // namespace vkpcnx