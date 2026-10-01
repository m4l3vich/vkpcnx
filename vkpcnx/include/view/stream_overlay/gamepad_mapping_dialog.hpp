#pragma once
#include "borealis/views/cells/cell_radio.hpp"
#include "borealis/views/scrolling_frame.hpp"
#include "view/stream_overlay/dialog_content.hpp"
#include <borealis.hpp>

namespace vkpcnx {
class GamepadMappingDialog : public DialogContent {
public:
  GamepadMappingDialog();
  BRLS_BIND(brls::ScrollingFrame, scrollingFrame, "gamepad_mapping_dialog/scrolling_frame");

  brls::ScrollingFrame *getScrollingFrame() override { return this->scrollingFrame; }
  brls::Box *getContentBox() override { return this->content; }

private:
  void changeMode(std::string newMode);
  void renderCurrentMode();

  BRLS_BIND(brls::Box, content, "gamepad_mapping_dialog/content");

  BRLS_BIND(brls::Image, literalPic, "gamepad_mapping_dialog/literal_pic");
  BRLS_BIND(brls::Image, positionalPic, "gamepad_mapping_dialog/positional_pic");

  BRLS_BIND(brls::Label, literalDesc, "gamepad_mapping_dialog/literal_desc");
  BRLS_BIND(brls::Label, positionalDesc, "gamepad_mapping_dialog/positional_desc");

  BRLS_BIND(brls::RadioCell, literalRadio, "gamepad_mapping_dialog/literal_radio");
  BRLS_BIND(brls::RadioCell, positionalRadio, "gamepad_mapping_dialog/positional_radio");
};
}; // namespace vkpcnx