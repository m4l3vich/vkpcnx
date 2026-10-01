#include "view/account_details.hpp"
#include "activity/stream_activity.hpp"
#include "core/settings.hpp"
#include "core/utils/config.hpp"
#include "core/utils/image.hpp"
#include "core/utils/iso8601.hpp"
#include "core/utils/lifecycle.hpp"
#include "core/vkpcapi.hpp"
#include "nanovg.h"

#include <charconv>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>

using namespace brls::literals;

namespace {
template <typename Setter>
void setIfChanged(std::string &cache, std::string value, Setter &&setter) {
  if (cache == value)
    return;
  cache = value;
  setter(cache);
}
} // namespace

namespace vkpcnx {
void AccountDetails::renderControlsInfo() {
  auto settings = Settings::instance();

  // Controls: mapping mode
  auto mappingMode = settings.get<std::string>("/controls/mapping_mode", "positional");
  std::string mappingModeName = std::unordered_map<std::string, std::string>{
    {"literal", "Буквальная"}, {"positional", "Позиционная"}
  }[mappingMode];

  setIfChanged(this->lastMappingModeName, mappingModeName, [this](auto &v) {
    this->gamepadMappingModeCell->setDetailText(v);
  });

  // Controls: gamepad pointer mode
  auto gamepadPointerEnabled = settings.get<bool>("/controls/gamepad_pointer", false);
  NVGcolor pointerDetailColor = brls::Application::getTheme().getColor(
    gamepadPointerEnabled ? "brls/slider/line_filled" : "brls/slider/line_empty"
  );

  setIfChanged(
    this->lastGamepadPointerText, gamepadPointerEnabled ? "Вкл" : "Выкл", [this](auto &v) {
      this->gamepadPointerCell->setDetailText(v);
    }
  );
  this->gamepadPointerCell->setDetailTextColor(pointerDetailColor);

  // Controls: touchscreen mode
  auto touchscreenMode = settings.get<std::string>("/controls/touchscreen_mode", "touchscreen");
  std::string touchscreenModeName = std::unordered_map<std::string, std::string>{
    {"trackpad", "Трекпад"}, {"touchscreen", "Тачскрин"}
  }[touchscreenMode];

  setIfChanged(this->lastTouchscreenModeText, touchscreenModeName, [this](auto &v) {
    this->touchscreenModeCell->setDetailText(v);
  });
}

void AccountDetails::openDialog(DialogContent *content) {
  brls::Dialog *dialog = new brls::Dialog(content);
  dialog->addButton("Сохранить", [] {});
  dialog->setCancelable(true);
  dialog->getAppletFrame()->setWidth(800);
  dialog->getAppletFrame()->setHeightPercentage(90.0f);
  dialog->setLastFocusedView(content->getScrollingFrame());
  dialog->open();
}

void AccountDetails::openLogoutDialog() {
  auto *content =
    static_cast<brls::Box *>(brls::View::createFromXMLResource("views/logout_dialog.xml"));
  brls::Dialog *dialog = new brls::Dialog(content);

  auto bind = [dialog, content](const std::string &id, std::function<void()> cb) {
    auto *button = content->getView(id);
    button->registerClickAction([dialog, cb](brls::View *) {
      dialog->dismiss(cb);
      return true;
    });
    return button;
  };
  bind("logout_dialog/wipe", [] {
    Settings::instance().wipe();
    vkpcnx::utils::relaunch();
  });
  bind("logout_dialog/keep", [] {
    Settings::instance().remove("account");
    vkpcnx::utils::relaunch();
  });
  auto *cancel = bind("logout_dialog/cancel", [] {});

  dialog->setCancelable(true);
  dialog->setLastFocusedView(cancel);
  dialog->open();
}

void AccountDetails::openControlsPlayground() {
  brls::Application::pushActivity(
    new StreamActivity(StreamActivity::Playground{}), brls::TransitionAnimation::FADE
  );
}

void AccountDetails::renderAccountDetails(const nlohmann::json &account) {
  auto userId = account.at("session").at("user_id").get<int>();
  auto name = account.at("name").get<std::string>();

  vkpcnx::utils::loadImageFromUrl(this->avatar, fmt::format(vkpcnx::utils::AVATAR_URL_FMT, userId));

  this->username->setText(name);

  auto plan = account.at("plan");
  if (plan.is_null()) {
    this->planName->setText("Нет подписки");
    this->planName->setText("Купите подписку в личном кабинете");
  } else {
    this->planName->setText("Тариф: " + plan.at("name").get<std::string>());
    std::string description;

    int planType = plan.at("plan_type").get<int>();
    switch (planType) {
    case 0: {
      int balance = account.at("minutes_balance").get<int>();
      int hours = balance / 60;
      int minutes = balance % 60;

      description = fmt::format("Осталось {:02} ч : {:02} м", hours, minutes);
      break;
    }
    case 1: {
      description = "Безлимит";
      break;
    }
    case 2: {
      int balance = account.at("minutes_balance").get<int>();
      int hours = balance / 60;
      int minutes = balance % 60;

      description = fmt::format("Осталось {:02} ч : {:02} м", hours, minutes);
      description += fmt::format("\n+ Ночной безлимит ({})", getUnlimitedTimespan());
      break;
    }
    }

    // Parse prolongation price from either
    // plan.price "123.00" or plan.prolongation_discount.price "123.00 руб."
    auto prolongPrice = plan.at("price").get<std::string_view>();
    if (plan.contains("prolongation_discount")) {
      prolongPrice = plan.at("prolongation_discount").at("price").get<std::string_view>();
    }

    size_t dot = prolongPrice.find('.');
    std::string_view intPart =
      (dot == std::string_view::npos) ? prolongPrice : prolongPrice.substr(0, dot);

    long prolongPriceInt = 0;
    std::from_chars(intPart.data(), intPart.data() + intPart.size(), prolongPriceInt);

    // Format prolongation price as "1 234 ₽"
    std::string digits = std::to_string(prolongPriceInt);
    std::string grouped;
    int count = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
      if (count != 0 && count % 3 == 0)
        grouped.push_back(' ');
      grouped.push_back(*it);
      ++count;
    }
    std::reverse(grouped.begin(), grouped.end());
    std::string prolongPriceFormatteed = grouped + " \u20BD"; // ₽

    if (planType > 0) {
      auto subscribedTill = account.at("subscribed_till").get<std::string>();
      brls::Logger::warning("test {}", subscribedTill);
      auto nextPaymentStr = vkpcnx::utils::Iso8601ToLocal(subscribedTill, "%d.%m.%Y");
      if (nextPaymentStr) {
        description +=
          fmt::format("\nСледующий платёж {} {}", prolongPriceFormatteed, *nextPaymentStr);
      }
    } else {
      description += fmt::format("\nСледующий платёж {}", prolongPriceFormatteed);
    }

    description += fmt::format(
      "\nАвтопродление {}", account.at("is_subscription_prolongs").get<bool>() ? "ВКЛ" : "ВЫКЛ"
    );

    this->planDescription->setText(description);
  }
}

std::string AccountDetails::getUnlimitedTimespan() const {
  constexpr int mskStartHour = 0;
  constexpr int mskEndHour = 8;
  constexpr int kMskOffsetSeconds = 3 * 3600;

  const std::time_t now = std::time(nullptr);
  std::tm localTm{};
  std::tm utcTm{};
#if defined(_WIN32)
  localtime_s(&localTm, &now);
  gmtime_s(&utcTm, &now);
#else
  localtime_r(&now, &localTm);
  gmtime_r(&now, &utcTm);
#endif
  const std::time_t localAsUtc = std::mktime(&localTm);
  const std::time_t utcAsUtc = std::mktime(&utcTm);
  const long localOffsetSeconds =
    static_cast<long>(localAsUtc - utcAsUtc) + (localTm.tm_isdst > 0 ? 3600 : 0);

  const long deltaSeconds = localOffsetSeconds - kMskOffsetSeconds;
  const int deltaHours = deltaSeconds / 3600;

  std::string timezoneStr;
  if (deltaHours > 0) {
    timezoneStr = "МСК+" + std::to_string(deltaHours);
  } else if (deltaHours < 0) {
    char zoneBuf[16];
    std::strftime(zoneBuf, sizeof(zoneBuf), "%Z", &localTm);
    timezoneStr = zoneBuf;
  } else {
    timezoneStr = "МСК";
  }

  auto fmtHourLocal = [&](int mskHour) {
    long totalMinutes = mskHour * 60 + deltaSeconds / 60;
    constexpr long kMinutesPerDay = 24 * 60;
    totalMinutes = ((totalMinutes % kMinutesPerDay) + kMinutesPerDay) % kMinutesPerDay;

    char buf[6];
    std::snprintf(buf, sizeof(buf), "%02ld:%02ld", totalMinutes / 60, totalMinutes % 60);
    return std::string(buf);
  };

  return fmtHourLocal(mskStartHour) + "-" + fmtHourLocal(mskEndHour) + " " + timezoneStr;
}

void AccountDetails::fetchServerLoad() {
  const NVGcolor loadLowColor = nvgRGB(0x3B, 0xB5, 0x4A);    // #3bb54a
  const NVGcolor loadMediumColor = nvgRGB(0xFD, 0xB9, 0x3E); // #fdb93e
  const NVGcolor loadHighColor = nvgRGB(0xDC, 0x00, 0x50);   // #dc0050

  constexpr float lowThreshold = 0.9;
  constexpr float mediumThreshold = 1.2;

  auto r = VKPCAPI::queueServerLoad();

  if (!r.contains("server_load")) {
    this->serverLoadLabel->setText("Не удалось загрузить :(");
    return;
  }

  float load = r.at("server_load").get<float>();
  std::string loadStr;
  if (load <= lowThreshold) {
    loadStr = "Низкая";
    this->serverLoadBar1->setColor(loadLowColor);
  } else if (load <= mediumThreshold) {
    loadStr = "Умеренная";
    this->serverLoadBar1->setColor(loadMediumColor);
    this->serverLoadBar2->setColor(loadMediumColor);
  } else {
    loadStr = "Высокая";
    this->serverLoadBar1->setColor(loadHighColor);
    this->serverLoadBar2->setColor(loadHighColor);
    this->serverLoadBar3->setColor(loadHighColor);
  }

  this->serverLoadLabel->setText(fmt::format("{} ({})", loadStr, load));
};
} // namespace vkpcnx