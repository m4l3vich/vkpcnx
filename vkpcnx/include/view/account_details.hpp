#pragma once
#include "core/account_info.hpp"
#include "core/diag/build_info.hpp"
#include "core/utils/thread.hpp"
#include "view/debug_report.hpp"
#include "view/feedback_qr_dialog.hpp"
#include "view/stream_overlay/dialog_content.hpp"
#include "view/stream_overlay/gamepad_mapping_dialog.hpp"
#include "view/stream_overlay/gamepad_pointer_dialog.hpp"
#include "view/stream_overlay/touchscreen_mode_dialog.hpp"
#include <borealis.hpp>

namespace vkpcnx {
class AccountDetails : public brls::Box {
public:
  AccountDetails() {
    this->inflateFromXMLRes("xml/views/account.xml");

    auto &accountInfo = AccountInfo::instance();

    accountInfoSub = accountInfo.updated().subscribe([this](const nlohmann::json &account) {
      renderAccountDetails(account);
    });

    if (accountInfo.get()->empty()) {
      accountInfo.fetch();
    } else {
      renderAccountDetails(accountInfo.get());
    }

    this->gamepadMappingModeCell->registerClickAction([this](auto &&) {
      this->openDialog(new GamepadMappingDialog());
      return true;
    });
    this->gamepadPointerCell->registerClickAction([this](auto &&) {
      this->openDialog(new GamepadPointerDialog());
      return true;
    });
    this->touchscreenModeCell->registerClickAction([this](auto &&) {
      this->openDialog(new TouchscreenModeDialog());
      return true;
    });
    this->controlsPlaygroundBtn->registerClickAction([this](auto &&) {
      this->openControlsPlayground();
      return true;
    });
    this->feedbackBtn->registerClickAction([](auto &&) {
      brls::Dialog *dialog = new brls::Dialog(new FeedbackQrDialog());
      dialog->addButton("Создать отчёт об ошибке", [] { openDebugReportDialog(); });
      dialog->addButton("Закрыть", [] {});
      dialog->setCancelable(true);
      dialog->getAppletFrame()->setWidth(800);
      dialog->open();
      return true;
    });
    this->logoutBtn->registerClickAction([this](auto &&) {
      this->openLogoutDialog();
      return true;
    });

    this->versionLabel->setText("vkpcnx " + diag::buildInfo().git);

    vkpcnx::utils::runDetached([this] { fetchServerLoad(); });
    this->controlsUpdLoopSub =
      brls::Application::getRunLoopEvent()->subscribe([this] { this->renderControlsInfo(); });
  };
  ~AccountDetails() {
    AccountInfo::instance().updated().unsubscribe(accountInfoSub);
    brls::Application::getRunLoopEvent()->unsubscribe(this->controlsUpdLoopSub);
  }

private:
  BRLS_BIND(brls::Box, container, "account_details/root");

  BRLS_BIND(brls::Image, avatar, "account_details/avatar");
  BRLS_BIND(brls::Label, username, "account_details/username");

  BRLS_BIND(brls::Label, planName, "account_details/plan/name");
  BRLS_BIND(brls::Label, planDescription, "account_details/plan/description");

  BRLS_BIND(brls::Label, serverLoadLabel, "account_details/server_load/label");
  BRLS_BIND(brls::Rectangle, serverLoadBar1, "account_details/server_load/bar_1");
  BRLS_BIND(brls::Rectangle, serverLoadBar2, "account_details/server_load/bar_2");
  BRLS_BIND(brls::Rectangle, serverLoadBar3, "account_details/server_load/bar_3");

  BRLS_BIND(brls::DetailCell, gamepadMappingModeCell, "account_details/gamepad_mapping_mode");
  BRLS_BIND(brls::DetailCell, gamepadPointerCell, "account_details/gamepad_pointer");
  BRLS_BIND(brls::DetailCell, touchscreenModeCell, "account_details/touchscreen_mode");
  BRLS_BIND(brls::Button, controlsPlaygroundBtn, "account_details/controls_playground");
  BRLS_BIND(brls::Button, feedbackBtn, "account_details/feedback");

  BRLS_BIND(brls::Button, logoutBtn, "account_details/logout");
  BRLS_BIND(brls::Label, versionLabel, "account_details/version");

  std::string getUnlimitedTimespan() const;
  void openDialog(DialogContent *content);
  void openLogoutDialog();
  void openControlsPlayground();
  void renderAccountDetails(const nlohmann::json &account);
  void fetchServerLoad();

  void renderControlsInfo();
  brls::Event<const nlohmann::json &>::Subscription accountInfoSub;
  brls::VoidEvent::Subscription controlsUpdLoopSub;
  std::string lastMappingModeName;
  std::string lastGamepadPointerText;
  std::string lastTouchscreenModeText;
};
}; // namespace vkpcnx