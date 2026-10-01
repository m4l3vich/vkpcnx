#pragma once
#include "view/stream_overlay/dialog_content.hpp"
#include <borealis.hpp>

namespace vkpcnx {
class FeedbackQrDialog : public DialogContent {
public:
  FeedbackQrDialog() { this->inflateFromXMLRes("xml/views/feedback_qr_dialog.xml"); };

  brls::ScrollingFrame *getScrollingFrame() override { return nullptr; }
  brls::Box *getContentBox() override { return nullptr; }
};
}; // namespace vkpcnx