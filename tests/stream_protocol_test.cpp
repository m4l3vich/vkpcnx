// Unit tests for the pure protocol pieces (no borealis / network).
// Build: see VKPCNX_BUILD_TESTS in CMakeLists.txt

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/stream/input_encoder.hpp"
#include "core/stream/play_url.hpp"
#include "core/stream/sdp_munging.hpp"
#include "core/stream/text_keystrokes.hpp"
#include "core/stream/wire.hpp"

using namespace vkpcnx::stream;

static int failures = 0;
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                  \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

static std::string hex(const std::vector<uint8_t> &v) {
  std::string s;
  char b[4];
  for (uint8_t c : v) {
    std::snprintf(b, sizeof b, "%02x", c);
    s += b;
  }
  return s;
}

static uint64_t fakeNow = 1000;
static uint64_t fakeClock() { return fakeNow; }

static void testWire() {
  auto f = encodeFrame(4);
  CHECK(hex(f) == "0400000004000000");
  auto d = decodeFrame(f);
  CHECK(d && d->type == 4 && d->payload.empty());
  auto g = encodeFrame(3880, "abc");
  CHECK(g.size() == 11 && g[0] == 7);
  auto e = decodeFrame(g);
  CHECK(e && e->type == 3880 && e->payload == "abc");
  g[0] = 9; // wrong length
  CHECK(!decodeFrame(g));
  CHECK(messageTypeName(4) == "M_KEEP_ALIVE");
}

static void testPlayUrl() {
  auto p = PlayUrl::parse(
    "playkey:///"
    "?host=cgw.clgrtc.ru&port=19000&token=abc%20d&gameId=42&session-id=c123&language=ru&fps=60&"
    "resolution=1920x1080&work-mode=1&allow-features=3"
  );
  CHECK(p.has_value());
  CHECK(p->host == "cgw.clgrtc.ru");
  CHECK(p->port == 19000);
  CHECK(p->token == "abc d");
  CHECK(p->gameId == 42);
  CHECK(p->playTokenId == 123);
  CHECK(p->language == "ru");
  CHECK(p->fps == 60);
  CHECK(p->resolutionWidth == 1920 && p->resolutionHeight == 1080);
  CHECK(p->workMode == PlayUrl::WorkMode::PingTest);
  CHECK(p->allowFeatures == 3);
  CHECK(p->authType() == PlayUrl::AuthType::GameToken);
  CHECK(p->managerUrl() == "wss://cgw.clgrtc.ru:19000");
  auto q = PlayUrl::parse("playkey:///?host=h&port=1&token=t");
  CHECK(q && q->authType() == PlayUrl::AuthType::SingleToken);
  CHECK(!PlayUrl::parse("https://example.com/?host=h&port=1"));
  CHECK(!PlayUrl::parse("playkey:///?host=h"));
}

// §8.8 worked example: press A (DIK 0x1E), token 0x11223344, ts 1000 µs
static void testWorkedExample() {
  InputTransmitter::setClockForTesting(fakeClock);
  fakeNow = 1000;
  InputTransmitter t;
  t.setToken(0x11223344);
  t.setCollectEventsFlag(true);
  t.sendKeyboardEvent(0x1E, true, 0);
  t.setCollectEventsFlag(false);
  auto msgs = t.flush(false);
  CHECK(msgs.size() == 1);
  const std::string expected =
    "01000000"                                                     // message_id
    "44332211"                                                     // token
    "0000000000000000"                                             // locale
    "00"                                                           // lock_keys
    "01"                                                           // device_mask
    "000000400000000000000000000000000000000000000000000000000000" // kbd bitmap
    "01"                                                           // event_count
    "01000000"                                                     // newest_event_id
    "e803000000000000"                                             // timestamp
    "00000000011e00";                                              // delta, KEY_PRESS, key, lock
  CHECK(msgs[0].size() == 68);
  CHECK(hex(msgs[0]) == expected);
  if (hex(msgs[0]) != expected)
    std::printf("  got      %s\n  expected %s\n", hex(msgs[0]).c_str(), expected.c_str());

  // Re-flush resends the same event with message_id 2
  auto again = t.flush(false);
  CHECK(again.size() == 1 && again[0][0] == 2 && again[0].size() == 68);
  CHECK(!t.empty());
  t.setLastConfirmedEventId(1);
  CHECK(t.empty());
  CHECK(t.flush(false).empty());
  InputTransmitter::setClockForTesting(nullptr);
}

static void testReleaseAllAndChunks() {
  InputTransmitter::setClockForTesting(fakeClock);
  fakeNow = 5000;
  InputTransmitter t;
  t.setToken(1);
  t.setCollectEventsFlag(true);
  t.sendKeyboardEvent(0x1E, true, 0);
  t.sendKeyboardEvent(0x1F, true, 0);
  t.sendMouseButton(pk::MOUSE_LEFT, true);
  t.sendGamepadButton(0, InputTransmitter::GamepadType::XInput, pk::PAD_A, true);
  t.sendGamepadPov(0, InputTransmitter::GamepadType::XInput, pk::POV_N);
  t.sendGamepadAxis(0, InputTransmitter::GamepadType::XInput, pk::AXIS_X, -32768);
  t.setCollectEventsFlag(false);
  CHECK(t.anyHeld());
  auto msgs = t.flush(true);
  CHECK(msgs.size() == 1);
  // 6 recorded + 2 key releases + 1 mouse release + 1 pad release + 1 pov + RELEASE_ALL = 12
  const auto &m = msgs[0];
  // header: 4+4+8+1+1 = 18, then kbd(30) + mouse(2) + pad0(2+24+1=27) = 77
  size_t off = 18;
  for (int i = 0; i < 30; i++)
    CHECK(m[off + i] == 0); // keys cleared by release-all
  off += 30;
  CHECK(m[off] == 0 && m[off + 1] == 0); // mouse buttons cleared
  off += 2;
  CHECK(m[off] == 0 && m[off + 1] == 0); // pad buttons cleared
  int32_t axisX;
  std::memcpy(&axisX, &m[off + 2], 4);
  CHECK(axisX == -32768);  // axes kept
  CHECK(m[off + 26] == 0); // pov cleared
  off += 27;
  CHECK(m[off] == 12); // event_count
  CHECK(!t.anyHeld());
  // Newest-first: the last event is RELEASE_ALL (0x0A)
  off += 1 + 4 + 8;
  CHECK(m[off + 4] == InputTransmitter::RELEASE_ALL);
  // Gamepad kind byte for pad 0, XInput, POV: 0x1B
  CHECK(m[off + 5 + 4] == 0x1B);

  // Chunking: 45 new events → 20, 20 then the newest 20 (which overlap)
  InputTransmitter c;
  c.setCollectEventsFlag(true);
  for (int i = 0; i < 45; i++)
    c.sendMouseScroll(120);
  auto chunks = c.flush(false);
  CHECK(chunks.size() == 3);
  // event_count is at offset 18 + mouse(2) = 20
  CHECK(chunks[0][20] == 20 && chunks[1][20] == 20 && chunks[2][20] == 20);
  uint32_t newestId;
  std::memcpy(&newestId, &chunks[2][21], 4);
  CHECK(newestId == 45);
  std::memcpy(&newestId, &chunks[0][21], 4);
  CHECK(newestId == 20);
  InputTransmitter::setClockForTesting(nullptr);
}

static void testSdpMunging() {
  std::string sdp =
    "v=0\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 96 97 98 99 100 101\r\n"
    "a=rtpmap:96 H264/90000\r\n"
    "a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
    "a=rtpmap:97 rtx/90000\r\n"
    "a=fmtp:97 apt=96\r\n"
    "a=rtpmap:98 H264/90000\r\n"
    "a=fmtp:98 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=64001f\r\n"
    "a=rtpmap:99 rtx/90000\r\n"
    "a=fmtp:99 apt=98\r\n"
    "a=rtpmap:100 H264/90000\r\n"
    "a=fmtp:100 level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=64001f\r\n"
    "a=rtpmap:101 VP8/90000\r\n";
  VideoSdpParams params;
  params.preferredProfile = H264Profile::High;
  params.startKbps = 8000;
  params.minKbps = 1000;
  params.maxKbps = 25000;
  std::string out = mungeVideoOffer(sdp, params);
  CHECK(out.find("m=video 9 UDP/TLS/RTP/SAVPF 98 96 97 99 100 101\r\n") != std::string::npos);
  CHECK(
    out.find(
      "a=fmtp:98 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=64001f\r\n"
      "a=fmtp:98 x-google-start-bitrate=8000\r\n"
      "a=fmtp:98 x-google-min-bitrate=1000\r\n"
      "a=fmtp:98 x-google-max-bitrate=25000\r\n"
      "a=fmtp:98 sps-pps-idr-in-keyframe=1\r\n"
    ) != std::string::npos
  );
  CHECK(out.find("a=fmtp:97 apt=96;rtx-time=125\r\n") != std::string::npos);
  CHECK(out.find("a=fmtp:99 apt=98;rtx-time=125\r\n") != std::string::npos);
  if (failures)
    std::printf("%s\n", out.c_str());
  std::string suffix = cloudGamingSuffix(60, 1920, 1080, 0);
  CHECK(
    suffix ==
    "cloudgaming:fps=60;streamwidth=1920;streamheight=1080;refframes=1;slices=1;rgbrange=0;"
  );
}

static void testTextKeystrokes() {
  auto ks = textToKeystrokes("aZ1!\n");
  CHECK(ks.size() == 5);
  CHECK(ks[0].dik == 30 && !ks[0].shift && std::strcmp(ks[0].locale, "en-US") == 0);
  CHECK(ks[1].dik == 44 && ks[1].shift);
  CHECK(ks[2].dik == 2 && !ks[2].shift);
  CHECK(ks[3].dik == 2 && ks[3].shift);
  CHECK(ks[4].dik == 28);

  // Cyrillic on ЙЦУКЕН: й=Q, Ё=Shift+`, №=Shift+3; '.' goes through en-US
  ks = textToKeystrokes("йЁ№. ");
  CHECK(ks.size() == 5);
  CHECK(ks[0].dik == 16 && !ks[0].shift && std::strcmp(ks[0].locale, "ru-RU") == 0);
  CHECK(ks[1].dik == 41 && ks[1].shift && std::strcmp(ks[1].locale, "ru-RU") == 0);
  CHECK(ks[2].dik == 4 && ks[2].shift && std::strcmp(ks[2].locale, "ru-RU") == 0);
  CHECK(ks[3].dik == 52 && !ks[3].shift && std::strcmp(ks[3].locale, "en-US") == 0);
  CHECK(ks[4].dik == 57);

  // Untypable characters are skipped, invalid UTF-8 doesn't loop or crash
  CHECK(textToKeystrokes("\xe2\x82\xac").empty()); // €
  CHECK(textToKeystrokes("\xff\xc3" "a").size() == 1);
}

int main() {
  testWire();
  testTextKeystrokes();
  testPlayUrl();
  testWorkedExample();
  testReleaseAllAndChunks();
  testSdpMunging();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("all tests passed\n");
  return 0;
}
