#pragma once
#include "borealis/core/activity.hpp"
#include "core/account_info.hpp"
#include <borealis.hpp>
#include <nlohmann/json.hpp>

namespace vkpcnx {
class CustomTitlebarActivity : public brls::Activity {
public:
  explicit CustomTitlebarActivity(std::string appletFrameId)
      : appletFrame(std::move(appletFrameId), this) {}

  ~CustomTitlebarActivity() {
    auto &account = AccountInfo::instance();
    account.updated().unsubscribe(this->accountSub);
  };

  void onContentAvailable() override final {
    this->setupTitleBar();
    onContentReady();
  }

protected:
  virtual void onContentReady() {};

private:
  brls::Event<const nlohmann::json &>::Subscription accountSub;
  brls::BoundView<brls::AppletFrame> appletFrame;

  void setupTitleBar();
};
} // namespace vkpcnx
