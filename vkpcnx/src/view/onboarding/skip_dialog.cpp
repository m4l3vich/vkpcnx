#include "view/onboarding/skip_dialog.hpp"
#include "core/settings.hpp"

namespace vkpcnx::Onboarding {
SkipDialog::SkipDialog() {
  this->inflateFromXMLRes("xml/views/onboarding/skip_dialog.xml");

  auto &settings = Settings::instance();

  auto mapping = settings.get<std::string>("/controls/mapping_mode", "positional");
  this->mappingMode->setText(mapping == "literal" ? "Буквальная" : "Позиционная");

  auto touchscreen = settings.get<std::string>("/controls/touchscreen_mode", "touchscreen");
  this->touchscreenMode->setText(touchscreen == "trackpad" ? "Трекпад" : "Тачскрин");

  bool pointer = settings.get<bool>("/controls/gamepad_pointer", false);
  this->gamepadPointer->setText(pointer ? "Вкл" : "Выкл");
}
} // namespace vkpcnx::Onboarding
