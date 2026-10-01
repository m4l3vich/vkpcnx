#include "activity/launch_queue_activity.hpp"

#include <chrono>
#include <cmath>
#include <future>
#include <thread>

#include "activity/stream_activity.hpp"
#include "core/stream/play_url.hpp"
#include "core/stream/zone_ping.hpp"
#include "core/utils/display.hpp"
#include "core/utils/image.hpp"
#include "core/utils/thread.hpp"
#include "core/vkpcapi.hpp"

using namespace vkpcnx;

namespace {

// No zone/region field exists in the protocol at this layer (§4.1's
// ZoneServer only carries host/dns names), so the resolved host's leading
// label is the closest thing to a region identifier.
std::string zoneServerLabel(const std::string &host) {
  auto dot = host.find('.');
  return dot == std::string::npos ? host : host.substr(0, dot);
}

// Only called for a successful probe; PING_FAILED is handled by the caller.
std::string pingText(int32_t pingMicros) {
  return std::to_string(static_cast<int>(std::lround(pingMicros / 1000.0))) + " мс";
}

} // namespace

void LaunchQueueActivity::onContentAvailable() {
  this->title->setText(this->game.name);
  vkpcnx::utils::loadImageFromUrl(this->image, game.pictureUrl);

  delayHandle = brls::delay(1500, [this] {
    delayHandle = 0;
    this->registerAction(
      "",
      brls::BUTTON_B,
      [this](brls::View *) {
        auto *dialog = new brls::Dialog("Отменить запуск?");
        dialog->addButton("Нет", [] {});
        dialog->addButton("Да, выйти", [this] {
          *alive = false;
          vkpcnx::utils::runDetached([] {
            try {
              VKPCAPI::leaveQueue();
            } catch (...) {
            }
          });
          brls::Application::popActivity(brls::TransitionAnimation::FADE);
        });
        dialog->setCancelable(true);
        dialog->open();
        return true;
      },
      true
    );

    vkpcnx::utils::runDetached([this, alive = this->alive] { this->initPlaySession(alive); });
  });
}

LaunchQueueActivity::~LaunchQueueActivity() {
  *alive = false;
  if (delayHandle)
    brls::cancelDelay(delayHandle);
}

void LaunchQueueActivity::setStatus(const std::string &t, const std::string &s) {
  brls::sync([this, alive = this->alive, t, s] {
    if (!*alive)
      return;
    if (!t.empty())
      animLabelTextChange(this->title, t, 1.0f);
    animLabelTextChange(this->subtitle, s, 0.75f);
  });
}

void LaunchQueueActivity::showError(const std::string &message) {
  brls::sync([alive = this->alive, message] {
    if (!*alive)
      return;
    *alive = false;
    auto *dialog = new brls::Dialog(message);
    dialog->addButton("OK", [] {
      brls::Application::popActivity(brls::TransitionAnimation::FADE);
    });
    dialog->setCancelable(false);
    dialog->open();
  });
}

bool LaunchQueueActivity::runPingTest(std::shared_ptr<std::atomic<bool>> alive) {
  nlohmann::json r = VKPCAPI::pingTest();
  std::string url = r.value("play_url", "");
  auto playUrl = stream::PlayUrl::parse(url);
  if (!playUrl) {
    brls::Logger::warning("LaunchQueue: ping test without a play_url, skipping");
    return false;
  }
  playUrl->workMode = stream::PlayUrl::WorkMode::PingTest;

  auto done = std::make_shared<std::promise<bool>>();
  auto future = done->get_future();
  brls::sync([this, alive, playUrl = *playUrl, done] {
    if (!*alive) {
      done->set_value(false);
      return;
    }
    pingSession = std::make_unique<stream::StreamSession>();
    stream::StreamSession::Config cfg;
    cfg.playUrl = playUrl;
    auto [monitorW, monitorH] = utils::displayPixelSize();
    cfg.manager.monitorWidth = monitorW;
    cfg.manager.monitorHeight = monitorH;
    auto finished = std::make_shared<bool>(false);
    pingSession->onStateChange = [done, finished](stream::StreamSession::State s) {
      if (*finished)
        return;
      if (s == stream::StreamSession::State::Ended || s == stream::StreamSession::State::Failed) {
        *finished = true;
        done->set_value(s == stream::StreamSession::State::Ended);
      }
    };
    pingSession->onPingProgress = [this, alive](int completed, int total) {
      if (!*alive)
        return;
      this->setSubtitleNoFade(
        "Проверка пинга (" + std::to_string(completed) + "/" + std::to_string(total) + ")"
      );
    };
    pingSession->onPingBestServer = [this, alive](const stream::ZonePing::Result &best) {
      if (!*alive)
        return;
      // "Best" is the lowest ping, but if every probe failed that's still
      // PING_FAILED sorted first — there's no real winner to show then.
      if (best.pingMicros == stream::ZonePing::PING_FAILED) {
        this->setSubtitleNoFade("Проверка не удалась, всё равно продолжаем");
        return;
      }
      this->setSubtitleNoFade(
        "Лучший сервер: " + zoneServerLabel(best.host) + " (" + pingText(best.pingMicros) + ")"
      );
    };
    pingSession->start(cfg);
  });

  bool ok = future.wait_for(std::chrono::seconds(60)) == std::future_status::ready && future.get();
  brls::sync([this, alive] {
    if (*alive)
      pingSession.reset();
  });
  return ok;
}

void LaunchQueueActivity::initPlaySession(std::shared_ptr<std::atomic<bool>> alive) {
  auto check = [&] { return alive->load(); };
  try {
    if (game.launcherId == 0) {
      showError("У этой игры нет доступного лаунчера");
      return;
    }
    setStatus("Вы в очереди", "Вход в очередь…");
    nlohmann::json q = VKPCAPI::enterQueue(game.launcherId);
    bool pingDone = false;

    while (check()) {
      std::string status = q.value("status", "");
      int number = q.value("number", 0);
      bool pingReady = q.value("is_ping_ready", true);
      brls::Logger::info(
        "LaunchQueue: status={} number={} ping_ready={}", status, number, pingReady
      );

      if (status == "allowed")
        break;
      if (status == "canceled") {
        showError("Запуск отменён сервером");
        return;
      }
      if (number > 0)
        setStatus("Вы в очереди", "Ваше место в очереди: " + std::to_string(number));
      else
        setStatus("Почти готово", "Ожидание сервера");

      // FIXME: for testing purposes ping test runs every session
      if (!pingDone) {
        // if (!pingReady && !pingDone) {
        setStatus("Проверка связи", "Измерение задержки до серверов…");
        pingDone = true;
        runPingTest(alive);
        if (!check())
          return;
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!check())
          return;
      }

      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (!check())
        return;
      q = VKPCAPI::getQueue();
      if (q.is_null())
        q = nlohmann::json::object();
    }
    if (!check())
      return;

    setStatus("Запуск", "Подготовка сессии");
    nlohmann::json run = VKPCAPI::runLauncher(game.launcherId);
    std::string url = run.value("play_url", "");
    auto playUrl = stream::PlayUrl::parse(url);
    if (!playUrl) {
      brls::Logger::error("LaunchQueue: bad play_url: {}", stream::PlayUrl::redact(url));
      showError("Сервер вернул некорректный адрес сессии");
      return;
    }
    brls::Logger::info("LaunchQueue: play_url: {}", stream::PlayUrl::redact(url));

    brls::sync([this, alive, playUrl = *playUrl] {
      if (!*alive)
        return;
      *alive = false;
      auto game = this->game;
      brls::Application::popActivity(brls::TransitionAnimation::NONE, [game, playUrl] {
        brls::Application::pushActivity(
          new StreamActivity(game, playUrl), brls::TransitionAnimation::FADE
        );
      });
    });
  } catch (const VKPCAPI::ApiException &e) {
    brls::Logger::error("LaunchQueue: API error {}: {}", e.status, e.body.dump());
    showError("Не удалось запустить игру: " + e.userMessage());
  } catch (const std::exception &e) {
    brls::Logger::error("LaunchQueue: {}", e.what());
    showError(std::string("Ошибка: ") + e.what());
  }
}

void LaunchQueueActivity::setSubtitleNoFade(const std::string &text) {
  // Stops any fade from animLabelTextChange in flight; its end callback is
  // guarded on `finished`, so this interruption (finished=false) is a no-op
  // there instead of overwriting `text` with stale content a moment later.
  subtitle->alpha.reset(0.75f);
  subtitle->setText(text);
}

void LaunchQueueActivity::animLabelTextChange(
  brls::Label *label, std::string newText, float targetAlpha = 1
) {
  if (label->getFullText() == newText)
    return;

  auto fadedOut = std::make_shared<bool>(false);

  label->alpha.setEndCallback([label, newText, targetAlpha, fadedOut](bool finished) {
    if (!finished || *fadedOut)
      return;

    *fadedOut = true;

    label->setText(newText);

    label->alpha.reset(0.0f);
    label->alpha.addStep(targetAlpha, 300, brls::EasingFunction::quadraticOut);
    label->alpha.start();
  });

  label->alpha.reset(label->alpha);
  label->alpha.addStep(0.0f, 100, brls::EasingFunction::quadraticOut);
  label->alpha.start();
}
