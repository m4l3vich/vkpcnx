#pragma once
#include <borealis.hpp>

namespace vkpcnx::Onboarding {
// Content of the "skip onboarding" confirmation: the controls settings that
// stay in effect, as currently stored (defaults when unset)
class SkipDialog : public brls::Box {
  BRLS_BIND(brls::Label, mappingMode, "onboarding/skip_dialog/mapping_mode");
  BRLS_BIND(brls::Label, touchscreenMode, "onboarding/skip_dialog/touchscreen_mode");
  BRLS_BIND(brls::Label, gamepadPointer, "onboarding/skip_dialog/gamepad_pointer");

public:
  SkipDialog();
};
} // namespace vkpcnx::Onboarding
