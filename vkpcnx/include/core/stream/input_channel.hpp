#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/stream/input_encoder.hpp"
#include "core/stream/input_sink.hpp"
#include "core/stream/webrtc_session.hpp"
#include "core/stream/wire.hpp"

namespace vkpcnx::stream {

// A decoded server cursor image (§7.6), RGBA top-down
struct CursorImage {
  uint32_t id = 0;
  int width = 0;
  int height = 0;
  int hotspotX = 0;
  int hotspotY = 0;
  std::vector<uint8_t> rgba; // width*height*4; empty ⇒ hide the cursor
};

// Decodes an ANI or raw cursor blob (§7.6); returns false if unparseable
bool decodeCursorBlob(const std::vector<uint8_t> &blob, CursorImage &out);

// Redundancy / backoff controller (§7.9)
class RedundancyController {
public:
  bool isExtraRedundant(uint32_t id, uint64_t nowMs) const {
    return id == lastSentId_ && nowMs < nextResendTimeMs_;
  }
  void commitSent(uint32_t id, uint64_t nowMs) {
    if (id != lastSentId_) {
      lastSentId_ = id;
      resendCount_ = 0;
    } else {
      resendCount_++;
    }
    int shift = resendCount_ > 16 ? 16 : resendCount_;
    uint64_t timeout =
      std::min<uint64_t>(static_cast<uint64_t>(resendTimeoutMs_) << shift, maxResendTimeoutMs_);
    nextResendTimeMs_ = nowMs + timeout;
  }
  void updateInterval(size_t bufferedAmount) {
    if (bufferedAmount < 6000) {
      resendTimeoutMs_ = 20;
      maxResendTimeoutMs_ = 40;
    } else {
      resendTimeoutMs_ = 50;
      maxResendTimeoutMs_ = 500;
    }
  }
  void reset() { *this = RedundancyController(); }

private:
  uint32_t lastSentId_ = 0;
  int resendCount_ = 0;
  int resendTimeoutMs_ = 20;
  int maxResendTimeoutMs_ = 40;
  uint64_t nextResendTimeMs_ = 0;
};

// Rate limiter for mouse moves / gamepad axes (§7.10), interval in ms
class InputRateLimiter {
public:
  void reset(uint64_t nowMs) {
    startMs_ = nowMs;
    level_ = 3; // 10 ms during the first 30 s
    lastChangeMs_ = nowMs;
  }
  void update(size_t bufferedAmount, uint64_t nowMs);
  double intervalMs() const { return LEVELS[level_]; }

private:
  static constexpr double LEVELS[] = {0.01, 2, 5, 10, 15, 25, 50};
  static int levelFor(size_t buffered) {
    if (buffered < 3000)
      return 0;
    if (buffered < 6000)
      return 1;
    if (buffered < 9000)
      return 2;
    if (buffered < 12000)
      return 3;
    if (buffered < 15000)
      return 4;
    if (buffered < 18000)
      return 5;
    return 6;
  }
  uint64_t startMs_ = 0;
  uint64_t lastChangeMs_ = 0;
  int level_ = 3;
};

// The "InputStream" DataChannel protocol of §7: init handshake, mouse
// settings, input reports, cursor and clipboard transfer, retransmission and
// the 4 ms process loop. Input events may be pushed from any thread; server
// messages are fed in from libdatachannel's thread via onMessage(). Callbacks
// run on whichever thread triggered them (never the UI thread).
class InputChannel : public InputSink {
public:
  static constexpr size_t MAX_BUFFERED_AMOUNT = 21000;
  static constexpr int PROCESS_INTERVAL_MS = 4;
  static constexpr int INIT_RETRY_INTERVAL_MS = 100;
  static constexpr uint32_t INPUT_PROTOCOL_VERSION = 2;
  static constexpr int CLIPBOARD_CHUNK = 1300;

  explicit InputChannel(WebRtcSession &rtc);
  ~InputChannel() override;

  std::function<void()> onTokenAccepted;
  std::function<void(const CursorImage &cursor)> onCursor;
  std::function<void(const std::string &text)> onClipboardText;
  std::function<void(double rttMs)> onInputRtt;
  std::function<void(const std::string &message)> onError;

  // DataChannel opened: begin CS_INPUT_INIT_V2 with the SC_SDP_ANSWER control token
  void start(uint32_t token);
  // Release everything, stop timers, clear the transmitter
  void stop();
  bool isStarted() const { return started_; }

  // Server → client message from the DataChannel
  void onMessage(const rtc::binary &message);

  // Rendered video rectangle in window pixels (§7.3); re-sends CS_MOUSE_SETTINGS
  void setWindowSize(int width, int height);
  // Focus / pointer lock (§9): losing focus releases everything (§8.6)
  void setFocus(bool focused) override;
  bool hasFocus() const { return focused_; }
  // Current lock-key state (bits of pk::LOCK_*), used for the sync events
  void setLockKeys(uint8_t lockKeys) override;

  // Input events (§9). `locale` is "en-US"/"ru-RU"/"" (keep previous)
  void keyEvent(uint8_t dik, bool pressed, const std::string &locale = {}) override;
  void mouseMove(int dx, int dy) override;
  // Jump the virtual absolute position (window space of §7.3) to x/y; the
  // difference from the current position is sent as the relative delta.
  void mouseMoveTo(int x, int y) override;
  void mouseButton(uint8_t button, bool pressed) override;
  void mouseWheel(int steps) override; // ±1 per notch
  void gamepadButton(int index, uint8_t button, bool pressed) override;
  void gamepadAxis(int index, uint8_t axis, int32_t value) override;
  void gamepadPov(int index, uint32_t pov) override;
  // "release everything" for the last gamepad disconnecting etc.
  void releaseAll() override;

  // Client → server clipboard text (§7.7)
  void sendClipboardText(const std::string &text) override;

  uint32_t token() const { return token_; }
  double lastInputRttMs() const { return lastRttMs_; }

private:
  struct ClipboardTransfer {
    uint32_t id = 0;
    uint32_t packetsCount = 0;
    uint32_t rawSize = 0;
    std::map<uint32_t, std::string> packets;
  };
  struct CursorTransfer {
    uint32_t id = 0;
    uint32_t packets = 0;
    uint32_t last = 0;
    uint32_t rawSize = 0;
    std::map<uint32_t, std::string> received;
  };

  static uint64_t nowMs();
  void processLoop();
  void sendFrame(MessageType type, const std::string &payload); // drops when congested
  void sendInit();
  void sendMouseSettingsLocked(uint64_t nowMs);
  void sendInputReportLocked();
  void flushLocked(bool sendHeld);
  void sendLockSyncLocked();
  void emitMouseMoveLocked(uint64_t nowMs);
  void emitGamepadAxesLocked(uint64_t nowMs);
  void handleInputReport(const std::string &payload);
  void handleWebRtcInputReport(const std::string &payload);
  void handleCursor(const std::string &payload);
  void handleClipboard(const std::string &payload);
  void resendClipboardLocked(uint64_t nowMs);

  WebRtcSession &rtc_;
  mutable std::mutex mutex_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> started_{false};
  std::atomic<bool> focused_{false};
  std::atomic<double> lastRttMs_{0};

  InputTransmitter tx_;
  uint32_t token_ = 0;
  uint64_t lastInitSentMs_ = 0;
  uint8_t lockKeys_ = 0;
  std::string lastLocale_ = "unspecified";

  // Mouse settings (§7.3)
  int windowWidth_ = 0, windowHeight_ = 0;
  uint32_t mouseSettingsId_ = 0;
  uint32_t confirmedMouseSettingsId_ = 0;
  RedundancyController mouseSettingsRedundancy_;

  // Mouse movement accumulation + virtual absolute position (§9.2)
  InputRateLimiter rateLimiter_;
  int pendingDx_ = 0, pendingDy_ = 0;
  double absX_ = 0, absY_ = 0;
  uint64_t lastMoveEmitMs_ = 0;
  uint16_t mouseButtonsDown_ = 0;

  // Gamepad axes sampled per interval (§9.3)
  std::array<std::array<int32_t, 6>, InputTransmitter::MAX_GAMEPADS> axisValues_{};
  std::array<std::array<bool, 6>, InputTransmitter::MAX_GAMEPADS> axisDirty_{};
  std::array<uint16_t, InputTransmitter::MAX_GAMEPADS> padButtonsDown_{};
  std::array<uint32_t, InputTransmitter::MAX_GAMEPADS> padPov_{};
  uint64_t lastAxisEmitMs_ = 0;

  RedundancyController inputRedundancy_;

  // Cursor (§7.6)
  std::map<uint32_t, CursorTransfer> cursorTransfers_; // incomplete image transfers
  std::map<uint32_t, CursorImage> cursorImages_;       // decoded images by id
  uint32_t notifiedCursorId_ = UINT32_MAX;             // last passed to onCursor
  bool notifiedCursorShown_ = false;
  uint32_t cursorId_ = 0;
  uint32_t cursorLast_ = 0;
  std::vector<uint32_t> cursorReceivedIds_;

  // Clipboard (§7.7)
  uint32_t clipboardCounter_ = 1; // ids start at 2
  ClipboardTransfer outgoingClipboard_;
  std::vector<std::string> outgoingClipboardPackets_;
  std::vector<bool> outgoingClipboardAcked_;
  RedundancyController clipboardRedundancy_;
  ClipboardTransfer incomingClipboard_;
  uint32_t clipboardConfirmedId_ = 0;
};

} // namespace vkpcnx::stream
