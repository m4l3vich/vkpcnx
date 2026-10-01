#pragma once
#include <borealis.hpp>

namespace vkpcnx {
class ObservableDialog : public brls::Dialog {
public:
  using brls::Dialog::Dialog;
  brls::VoidEvent closeEvent;

  void dismiss(std::function<void(void)> cb = [] {}) override {
    this->closeEvent.fire();
    brls::Dialog::dismiss(cb);
  }
};
}; // namespace vkpcnx