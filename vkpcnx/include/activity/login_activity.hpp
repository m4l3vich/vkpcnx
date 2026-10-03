#pragma once
#include "activity/main_activity.hpp"
#include "core/settings.hpp"
#include "core/utils/thread.hpp"
#include "view/debug_report.hpp"
#include "view/title_bar.hpp"
#include <borealis.hpp>
#include <ctime>

void initAuth(brls::Activity *activity);
void pollAuth(brls::Activity *activity, std::string pullToken, std::time_t startTimestamp);

class LoginActivity : public vkpcnx::CustomTitlebarActivity {
public:
  LoginActivity() : CustomTitlebarActivity("login/root") {}

  CONTENT_FROM_XML_RES("activity/login.xml");
  BRLS_BIND(brls::AppletFrame, appletFrame, "login/root");

  void onContentReady() override {
    if (Settings::instance().has("account")) {
      // Root activity can't be popped; stack the main screen on top instead
      brls::Application::pushActivity(new MainActivity(), brls::TransitionAnimation::NONE);
      return;
    }

    // Login problems are worth a report too, and the feedback dialog with
    // its report button is only reachable once logged in
    appletFrame->registerAction("Отчёт об ошибке", brls::BUTTON_Y, [](brls::View *) {
      vkpcnx::openDebugReportDialog();
      return true;
    });
    registerExitAction(); // + quits from the root screens only, see MainActivity

    vkpcnx::utils::runDetached([this] { initAuth(this); });
  }
};