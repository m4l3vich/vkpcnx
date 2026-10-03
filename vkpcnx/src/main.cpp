#include <borealis.hpp>

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

#include "activity/login_activity.hpp"
#include "activity/main_activity.hpp"
#include "activity/stream_activity.hpp"
#include "core/diag/build_info.hpp"
#include "core/diag/crash_handler.hpp"
#include "core/diag/log.hpp"
#include "core/diag/report.hpp"
#include "core/http.hpp"
#include "core/platform/switch_heap.hpp"
#include "core/settings.hpp"
#include "core/utils/lifecycle.hpp"
#include "view/animated_webp.hpp"
#include "view/debug_report.hpp"
#include "view/fill_scrolling_frame.hpp"
#include "view/onscreen_keyboard.hpp"
#include "view/qrcode.hpp"
#include "view/stream_view.hpp"
#include "view/striped_list.hpp"

#include <glad/glad.h>

int main(int argc, char *argv[]) {
  // First: everything below may log, and crashes need the log file open
  vkpcnx::diag::initLogging(argc, argv);
  vkpcnx::diag::installCrashHandlers();

  vkpcnx::utils::setExecPath(argv[0]);
#ifdef VKPCNX_HAS_DEBUG_KEYS
  new vkpcnx::utils::DebugKeyListener();
#endif

  // --test-crash=segv|abort|throw crashes on purpose once the UI is up, to
  // check the crash section, the report offer and symbolization end to end
  std::string_view testCrash;
  for (int i = 1; i < argc; i++) {
    std::string_view arg(argv[i]);
    if (arg == "--create-report") {
      // For when the app can't get as far as its UI
      auto r = vkpcnx::diag::createReport("command line");
      std::printf("%s\n", r.ok ? r.path.c_str() : r.error.c_str());
      vkpcnx::diag::shutdownLogging();
      return r.ok ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (arg == "--sw-decode")
      setenv("VKPCNX_SW_DECODE", "1", 1); // read by VideoDecoder::open()
    else if (arg.rfind("--test-crash=", 0) == 0)
      testCrash = arg.substr(13);
  }

#ifdef __SWITCH__
  if (const auto &fix = heapFixup(); fix.count > 0) {
    brls::Logger::warning(
      "heap: inherited heap {:#x}..{:#x} has {} unusable block(s), using {}..{}", fix.heapStart,
      fix.heapEnd, fix.count, static_cast<void *>(fake_heap_start),
      static_cast<void *>(fake_heap_end)
    );
    for (int i = 0; i < fix.count && i < HeapFixup::kMaxBad; i++)
      brls::Logger::warning(
        "heap:   {:#x}..{:#x} perm={:#x} attr={:#x}", fix.bad[i].start, fix.bad[i].end,
        fix.bad[i].perm, fix.bad[i].attr
      );
  }
#endif

#ifdef __SWITCH__
  // borealis renders all text with the zh-Hans shared font on Switch, which draws
  // Cyrillic full-width. Its custom font replaces it, so use Nintendo's standard
  // font (proportional Latin + Cyrillic); CJK and icon fallbacks stay attached.
  brls::FontLoader::USER_FONT_PATH = BRLS_ASSET("font/switch_font.ttf");
#endif

  if (!brls::Application::init()) {
    brls::Logger::error("Application::init failed");
    return EXIT_FAILURE;
  }

  brls::Application::registerXMLView("vkpcnx:AnimatedWebp", vkpcnx::AnimatedWebp::create);
  brls::Application::registerXMLView(
    "vkpcnx:FillScrollingFrame", vkpcnx::FillScrollingFrame::create
  );
  brls::Application::registerXMLView("vkpcnx:OnscreenKeyboard", [] {
    return new vkpcnx::OnscreenKeyboard();
  });
  brls::Application::registerXMLView("vkpcnx:QrCode", vkpcnx::QrCode::create);
  brls::Application::registerXMLView("vkpcnx:StreamView", [] { return new vkpcnx::StreamView(); });
  brls::Application::registerXMLView("vkpcnx:StripedList", vkpcnx::StripedList::create);
  brls::Application::registerXMLView("vkpcnx:StripedListRow", vkpcnx::StripedListRow::create);

  Settings::instance().load();
  Http::init();

  brls::Application::createWindow("VK Play Cloud");

  brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);

  auto glString = [](GLenum name) {
    auto *s = reinterpret_cast<const char *>(glGetString(name));
    return std::string(s ? s : "?");
  };
  vkpcnx::diag::setSystemInfo(
    "gpu", glString(GL_RENDERER) + " (" + glString(GL_VENDOR) + ", " + glString(GL_VERSION) + ")"
  );

  // Development aid: jump straight into a stream with a known play_url
  // (e.g. from tests/mock_server.py), bypassing login and the REST queue.
  if (const char *devPlayUrl = std::getenv("VKPCNX_PLAY_URL")) {
    auto playUrl = vkpcnx::stream::PlayUrl::parse(devPlayUrl);
    if (!playUrl) {
      brls::Logger::error("VKPCNX_PLAY_URL is not a valid playkey:// URL");
      return EXIT_FAILURE;
    }
    brls::Logger::info("VKPCNX_PLAY_URL: {}", vkpcnx::stream::PlayUrl::redact(devPlayUrl));
    vkpcnx::GameCard::Game game;
    game.name = "VKPCNX_PLAY_URL";
    brls::Application::pushActivity(new StreamActivity(game, *playUrl));
  } else if (Settings::instance().has("account")) {
    brls::Application::pushActivity(new MainActivity());
  } else {
    brls::Application::pushActivity(new LoginActivity());
  }

  vkpcnx::offerReportAfterUncleanExit();

  if (!testCrash.empty())
    brls::delay(2000, [testCrash] {
      brls::Logger::warning("--test-crash={}: crashing on purpose", testCrash);
      if (testCrash == "throw")
        throw std::runtime_error("--test-crash");
      if (testCrash == "abort")
        std::abort();
      // A plain null write is UB that GCC deletes; a volatile store can't be
      volatile int *volatile target = nullptr;
      *target = 1;
    });

  brls::Logger::info("entering main loop");
  while (brls::Application::mainLoop())
    vkpcnx::diag::heartbeat();
  brls::Logger::info("main loop exited");

  Http::shutdown();
  vkpcnx::diag::shutdownLogging();
  return EXIT_SUCCESS;
}
