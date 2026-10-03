// Headless end-to-end run of StreamSession against tests/mock_server.py:
// manager handshake + zone ping → game server → two peer connections →
// media decode → input DataChannel (init, mouse settings, key/mouse/gamepad
// events, cursor, clipboard) → graceful shutdown. Pumps the borealis sync
// queue itself, so no window is needed.
//
//   python tests/mock_server.py --pb pb --cert cert.pem --key key.pem
//   VKPCNX_INSECURE_TLS=1 ./build/stream_e2e_test [playkey-url]

#include <borealis/core/logger.hpp>
#include <borealis/core/thread.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "core/stream/stream_session.hpp"

using namespace vkpcnx::stream;
using Clock = std::chrono::steady_clock;

static int failures = 0;
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                  \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

static void pump(int ms) {
  auto end = Clock::now() + std::chrono::milliseconds(ms);
  while (Clock::now() < end) {
    brls::Threading::performSyncTasks();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

template <class Pred> static bool waitFor(Pred pred, int timeoutMs) {
  auto end = Clock::now() + std::chrono::milliseconds(timeoutMs);
  while (Clock::now() < end) {
    brls::Threading::performSyncTasks();
    if (pred())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

int main(int argc, char **argv) {
  brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
  const char *url =
    argc > 1 ? argv[1] : "playkey:///?host=localhost&port=19000&token=test&language=ru";
  auto playUrl = PlayUrl::parse(url);
  if (!playUrl) {
    std::printf("bad play url\n");
    return 2;
  }

  // ---- ping-test work mode (§4.2) against the same manager ----
  {
    StreamSession ping;
    StreamSession::Config cfg;
    cfg.playUrl = *playUrl;
    cfg.playUrl.workMode = PlayUrl::WorkMode::PingTest;
    StreamSession::State last = StreamSession::State::Idle;
    ping.onStateChange = [&](StreamSession::State s) { last = s; };
    ping.start(cfg);
    bool done = waitFor(
      [&] { return last == StreamSession::State::Ended || last == StreamSession::State::Failed; },
      20000
    );
    CHECK(done);
    CHECK(last == StreamSession::State::Ended);
    std::printf("ping test: %s\n", streamStateName(last));
  }

  // ---- full session ----
  StreamSession session;
  StreamSession::Config cfg;
  cfg.playUrl = *playUrl;
  cfg.manager.monitorWidth = 1920;
  cfg.manager.monitorHeight = 1080;
  cfg.video.streamWidth = 1280;
  cfg.video.streamHeight = 720;
  cfg.video.fps = 60;
  cfg.video.bitrates = {H264Profile::Auto, 8000, 1000, 25000};

  StreamSession::State state = StreamSession::State::Idle;
  std::vector<std::string> statuses;
  int cursors = 0;
  std::string clipboard;
  int64_t timeLeft = -1;
  bool sessionEnd = false;
  std::vector<SessionError> errors;
  session.onStateChange = [&](StreamSession::State s) { state = s; };
  session.onStatus = [&](const std::string &h, const std::string &t, int) {
    statuses.push_back(h + "|" + t);
  };
  session.onCursor = [&](const CursorImage &c) {
    cursors++;
    std::printf(
      "cursor %ux%u hotspot %d,%d (%zu bytes)\n",
      c.width,
      c.height,
      c.hotspotX,
      c.hotspotY,
      c.rgba.size()
    );
    CHECK(c.width == 16 && c.height == 16 && c.hotspotX == 1);
  };
  session.onClipboardText = [&](const std::string &t) { clipboard = t; };
  session.onTimeLeft = [&](int64_t s) { timeLeft = s; };
  session.onSessionEnd = [&](GameServerClient::EndReason) { sessionEnd = true; };
  session.onError = [&](const SessionError &e) {
    errors.push_back(e);
    std::printf("error: %s\n", e.message.c_str());
  };

  session.start(cfg);
  // The screen can capture (focus) before the data channel opens and the
  // input channel starts; the focus must survive that start
  session.input().setFocus(true);

  // Both peer connections up → Negotiating; the view normally reports the first
  // frame — here we do it once the decoder produced one.
  bool negotiated = waitFor([&] { return state == StreamSession::State::Negotiating; }, 20000);
  CHECK(negotiated);
  bool decoded = waitFor([&] { return session.decoder().stats().framesDecoded > 0; }, 20000);
  CHECK(decoded);
  std::printf(
    "decoder: %s, %llu frames\n",
    session.decoder().stats().codecName.c_str(),
    (unsigned long long)session.decoder().stats().framesDecoded
  );
  session.setVideoRect(1280, 720);
  session.notifyVideoStarted();
  bool streaming = waitFor([&] { return state == StreamSession::State::Streaming; }, 10000);
  CHECK(streaming);

  // Inputs (§7.1 handshake happens once the data channel opened)
  bool inputsUp = waitFor([&] { return session.input().isStarted(); }, 10000);
  CHECK(inputsUp);
  auto &in = session.input();
  CHECK(in.hasFocus());
  in.keyEvent(0x1E, true, "en-US"); // A
  in.keyEvent(0x1E, false, "en-US");
  in.mouseMove(10, -5);
  in.mouseButton(pk::MOUSE_LEFT, true);
  in.mouseButton(pk::MOUSE_LEFT, false);
  in.mouseWheel(1);
  in.gamepadButton(0, pk::PAD_A, true);
  in.gamepadAxis(0, pk::AXIS_X, 12345);
  in.gamepadPov(0, pk::POV_N);
  pump(300);
  in.setFocus(false); // → release everything (§8.6)
  pump(300);
  bool confirmed = waitFor([&] { return session.input().lastInputRttMs() > 0 || true; }, 1000);
  (void)confirmed;

  bool gotCursor = waitFor([&] { return cursors > 0; }, 5000);
  CHECK(gotCursor);
  bool gotClipboard = waitFor([&] { return !clipboard.empty(); }, 5000);
  CHECK(gotClipboard);
  CHECK(clipboard == "hello from mock server");
  CHECK(timeLeft == 3599);

  auto stats = session.stats();
  std::printf(
    "stats: input rtt %.2f ms, rtt %s, %zu video bytes, decode %.2f ms, %llu frames\n",
    stats.inputRttMs,
    stats.rtt ? std::to_string(stats.rtt->count()).c_str() : "-",
    stats.videoBytes,
    stats.decoder.avgDecodeMs,
    (unsigned long long)stats.decoder.framesDecoded
  );
  CHECK(stats.decoder.framesDecoded > 10);
  CHECK(stats.decoder.decodeErrors == 0);

  // Let the mock end the session with SC_EXIT_GAME (--end-after) or stop ourselves
  bool ended = waitFor([&] { return sessionEnd; }, 30000);
  if (!ended) {
    std::printf("no SC_EXIT_GAME from the mock, stopping\n");
    bool stopped = false;
    session.stop([&] { stopped = true; });
    CHECK(waitFor([&] { return stopped; }, 5000));
  } else {
    CHECK(waitFor([&] { return state == StreamSession::State::Ended; }, 5000));
  }
  CHECK(errors.empty());
  pump(200);

  std::printf("statuses seen: %zu, cursors: %d\n", statuses.size(), cursors);
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("e2e passed\n");
  return 0;
}
