#pragma once
#include <atomic>
#include <borealis.hpp>
#include <memory>

#include "core/stream/stream_session.hpp"
#include "view/game_card.hpp"

// Queue → (ping test) → run → StreamActivity (protocol doc §2 / §12 step 1)
class LaunchQueueActivity : public brls::Activity {
public:
  explicit LaunchQueueActivity(vkpcnx::GameCard::Game game) : game(std::move(game)) {}
  ~LaunchQueueActivity() override;

  CONTENT_FROM_XML_RES("activity/launch_queue.xml");
  BRLS_BIND(brls::AppletFrame, appletFrame, "launch_queue/root");

  void onContentAvailable() override;

private:
  BRLS_BIND(brls::Image, image, "launch_queue/image");
  BRLS_BIND(brls::Label, title, "launch_queue/title");
  BRLS_BIND(brls::Label, subtitle, "launch_queue/subtitle");

  size_t delayHandle = 0;
  vkpcnx::GameCard::Game game;
  // Shared with the worker thread; cleared when the activity goes away
  std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
  std::unique_ptr<vkpcnx::stream::StreamSession> pingSession;

  void initPlaySession(std::shared_ptr<std::atomic<bool>> alive);
  // Runs a ping-test session on the UI thread; blocks the calling worker thread
  bool runPingTest(std::shared_ptr<std::atomic<bool>> alive);
  void setStatus(const std::string &title, const std::string &subtitle);
  void showError(const std::string &message);
  void animLabelTextChange(brls::Label *label, std::string newText, float targetAlpha);
  // Updates the subtitle immediately, with no fade; also cancels any
  // in-flight animLabelTextChange so its delayed setText can't clobber this
  void setSubtitleNoFade(const std::string &text);
};
