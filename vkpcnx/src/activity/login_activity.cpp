#include "activity/login_activity.hpp"
#include "activity/onboarding_activity.hpp"
#include "core/http.hpp"
#include "core/utils/config.hpp"
#include "core/utils/device.hpp"
#include "view/qrcode.hpp"
#include <ctime>

void initAuth(brls::Activity *activity) {
  // TODO: error handling
  brls::Logger::info("init auth, target id={}", vkpcnx::utils::getDeviceUUID());
  Http::Response r = Http::postJson(
    vkpcnx::utils::AUTH_API_BASE + "/tvauth/forward/init",
    nlohmann::json{{"target", vkpcnx::utils::getDeviceUUID()}}
  );

  if (!r.ok()) {
    brls::Logger::error("auth init failed: status={} error={} body={}", r.status, r.error, r.body);
    return;
  }

  try {
    auto resp = r.json(); // TODO: read status and error for err handling
    auto pairingCode = resp.at("code").get<std::string>();
    auto pullToken = resp.at("token").get<std::string>();

    std::string qrLink = vkpcnx::utils::PAIRING_QR_LINK_PREFIX + pairingCode;
    brls::Logger::info("qr code link={}", qrLink);

    // Views must only be touched on the UI thread: setText() relayouts the tree
    // (Yoga), and doing that here races the UI thread's own layout pass.
    brls::sync([activity, qrLink, pairingCode] {
      auto *qrcode = static_cast<vkpcnx::QrCode *>(activity->getView("login/qrcode"));
      qrcode->setText(qrLink);

      auto *pairCodeLabel = static_cast<brls::Label *>(activity->getView("login/pairCode"));
      pairCodeLabel->setText(pairingCode);

      activity->getView("login/spinner")->setVisibility(brls::Visibility::GONE);
      activity->getView("login/content")->setVisibility(brls::Visibility::VISIBLE);
    });

    pollAuth(activity, pullToken, std::time(nullptr));
  } catch (const nlohmann::json::exception &) {
    brls::Logger::error(
      "auth init failed: invalid json: status={} error={} body={}", r.status, r.error, r.body
    );
  }
}

void pollAuth(brls::Activity *activity, std::string pullToken, std::time_t startTimestamp) {
  // Loop rather than recurse: polling every 2 s for the token's lifetime would
  // otherwise pile up ~900 frames on a small worker-thread stack.
  while (true) {
    if (std::difftime(std::time(nullptr), startTimestamp) >
        vkpcnx::utils::AUTH_PULL_TOKEN_LIFETIME) {
      brls::Logger::warning("auth token expired, starting over");
      return initAuth(activity);
    }

    Http::Response pollResp = Http::postJson(
      vkpcnx::utils::AUTH_API_BASE + "/tvauth/forward/verify", nlohmann::json{{"token", pullToken}}
    );

    if (pollResp.status == 0) {
      brls::Logger::error("auth poll request failed, status=0, error={}", pollResp.error);
      return;
    }

    try {
      std::string oauthCode = pollResp.json().at("oauth2_code").get<std::string>();
      brls::Logger::info("got oauth2_code, performing exchange; code={}", oauthCode);

      Http::Response exchangeResp = Http::postJson(
        vkpcnx::utils::OAUTH_TOKEN_ENDPOINT,
        nlohmann::json{{"client_id", vkpcnx::utils::OAUTH_CLIENT_ID}, {"code", oauthCode}}
      );

      auto account = exchangeResp.json();
      if (!exchangeResp.ok() || !account.is_object() || !account.contains("access_token")) {
        brls::Logger::error(
          "token exchange failed: status={} error={} body={}",
          exchangeResp.status,
          exchangeResp.error,
          exchangeResp.body
        );
        return;
      }

      brls::Logger::info("token exchange ok, logged in");
      Settings::instance().set("account", account);
      // LoginActivity is the root of the stack, and borealis never pops the root
      // activity (popActivity returns false without running its callback), so
      // stack the main screen and onboarding on top of it instead. Activity
      // changes must happen on the UI thread.
      brls::sync([] {
        brls::Application::pushActivity(new MainActivity(), brls::TransitionAnimation::NONE);
        brls::Application::pushActivity(new OnboardingActivity());
      });
      return;
    } catch (const nlohmann::json::exception &) {
      // not authorized yet
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
}
