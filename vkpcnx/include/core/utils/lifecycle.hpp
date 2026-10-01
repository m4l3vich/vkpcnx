#pragma once
#include <borealis.hpp>

#include "activity/onboarding_activity.hpp"
#include "activity/stream_activity.hpp"
#include "borealis/core/application.hpp"
#include "view/side_panel.hpp"
#include "view/stream_overlay.hpp"
#include <atomic>
#include <string>
#include <thread>

// The terminal debug keys need a POSIX tty (no termios in libnx or on Windows)
#if !defined(__SWITCH__) && !defined(_WIN32)
#define VKPCNX_HAS_DEBUG_KEYS 1
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace vkpcnx::utils {
void setExecPath(const std::string &path);
void relaunch();

#ifdef VKPCNX_HAS_DEBUG_KEYS
class DebugKeyListener {
  termios oldTerm{};
  std::atomic<bool> running{true};
  std::thread t;

  void handleKey(char c) {
    switch (c) {
    case 'o':
      brls::Logger::warning("'o' pressed — switching to OnboardingActivity");
      brls::sync([] { brls::Application::pushActivity(new OnboardingActivity()); });
      break;
    case 's':
      brls::Logger::warning("'s' pressed — opening StreamOverlay (NO DATA WILL BE PROVIDED)");
      brls::sync([] {
        auto content = new StreamOverlay();
        vkpcnx::SidePanel *panel = new vkpcnx::SidePanel(content, 480);
        panel->open();
      });
      break;
    case 'p':
      brls::Logger::warning("'p' pressed — opening StreamActvity::Playground");
      brls::Application::pushActivity(new StreamActivity(StreamActivity::Playground{}));
      break;
    }
  }

  void loop() {
    pollfd pfd{STDIN_FILENO, POLLIN, 0};
    while (running) {
      if (poll(&pfd, 1, 100) > 0) {
        char c;
        if (read(STDIN_FILENO, &c, 1) == 1)
          handleKey(c);
      }
    }
  }

public:
  DebugKeyListener() {
    tcgetattr(STDIN_FILENO, &oldTerm);
    termios raw = oldTerm;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    t = std::thread(&DebugKeyListener::loop, this);
  }

  ~DebugKeyListener() {
    running = false;
    if (t.joinable())
      t.join();
    tcsetattr(STDIN_FILENO, TCSANOW, &oldTerm);
  }

  DebugKeyListener(const DebugKeyListener &) = delete;
  DebugKeyListener &operator=(const DebugKeyListener &) = delete;
};
#endif
} // namespace vkpcnx::utils
