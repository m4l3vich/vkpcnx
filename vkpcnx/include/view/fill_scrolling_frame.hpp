#pragma once
#include <borealis.hpp>

namespace vkpcnx {

/// ScrollingFrame whose content is always at least as tall as the frame.
///
/// Borealis lays the content view out detached from the frame, so the frame's
/// justifyContent/alignItems never reach it and percentage heights don't
/// resolve. Pinning the content's min height to the frame lets the content
/// centre itself (justifyContent="center") while still scrolling once it
/// outgrows the frame.
///
/// The frame's own layout is usually cached by the time content is set, so
/// onLayout alone won't fire — the min height is also applied on assignment.
/// ScrollingFrame::setContentView isn't virtual: call it through a
/// FillScrollingFrame pointer (or via addView), not a brls::ScrollingFrame one.
class FillScrollingFrame : public brls::ScrollingFrame {
public:
  void setContentView(brls::View *view) {
    brls::ScrollingFrame::setContentView(view);
    this->fitContent();
  }

  void addView(brls::View *view) override { this->setContentView(view); }

  void onLayout() override {
    this->fitContent();
    brls::ScrollingFrame::onLayout();
  }

  static brls::View *create() { return new FillScrollingFrame(); }

private:
  void fitContent() {
    if (this->contentView)
      this->contentView->setMinHeight(this->getHeight());
  }
};

} // namespace vkpcnx
