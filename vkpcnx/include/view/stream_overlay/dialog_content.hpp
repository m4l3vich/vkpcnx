#pragma once
#include <borealis.hpp>

namespace vkpcnx {
class DialogContent : public brls::Box {
public:
  virtual brls::ScrollingFrame *getScrollingFrame() = 0;
  virtual brls::Box *getContentBox() = 0;

  brls::View *getNextFocus(brls::FocusDirection direction, brls::View *currentView) override {
    brls::ScrollingFrame *scrollingFrame = this->getScrollingFrame();
    if (currentView == scrollingFrame) {
      float offset = scrollingFrame->getContentOffsetY();
      float bottomLimit = this->getContentBox()->getHeight() - scrollingFrame->getHeight();
      bool canScroll = (direction == brls::FocusDirection::DOWN && offset < bottomLimit - 0.01f) ||
                       (direction == brls::FocusDirection::UP && offset > 0.01f);
      if (canScroll)
        return scrollingFrame;
    }
    return brls::Box::getNextFocus(direction, currentView);
  }
};
} // namespace vkpcnx
