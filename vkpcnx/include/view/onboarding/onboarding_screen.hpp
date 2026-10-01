#pragma once
#include <borealis.hpp>

namespace vkpcnx {

class OnboardingNavigator {
public:
  virtual ~OnboardingNavigator() = default;
  virtual void next() = 0;
  virtual void back() = 0;
  virtual void finish() = 0;
  virtual void setTitle(std::string title) = 0;
};

class OnboardingScreen : public brls::Box {
public:
  explicit OnboardingScreen(const std::string &res) {
    this->inflateFromXMLRes(res);
    this->registerAction("Назад", brls::BUTTON_B, [this](brls::View *) {
      this->back();
      return true;
    });
  }

  void setNavigator(OnboardingNavigator *nav) {
    this->navigator = nav;
    if (navigator && !this->title.empty()) {
      this->navigator->setTitle(this->title);
    }
  }

protected:
  void next() {
    if (navigator)
      navigator->next();
  }
  void back() {
    if (navigator)
      navigator->back();
  }
  void finish() {
    if (navigator)
      navigator->finish();
  }
  void setTitle(std::string title) {
    this->title = title;
    if (navigator)
      navigator->setTitle(title);
  }

private:
  OnboardingNavigator *navigator = nullptr;
  std::string title;
};

} // namespace vkpcnx