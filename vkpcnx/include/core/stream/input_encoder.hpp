#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace vkpcnx::stream {

// Playkey scan codes / ids used by the input protocol (§9)
namespace pk {
constexpr uint8_t KEY_UNASSIGNED = 0;

enum MouseButton : uint8_t {
  MOUSE_LEFT = 0,
  MOUSE_RIGHT = 1,
  MOUSE_MIDDLE = 2,
  MOUSE_SIDE = 3,
  MOUSE_EXTRA = 4,
  MOUSE_FORWARD = 5,
  MOUSE_BACK = 6,
  MOUSE_EXTRA2 = 7,
  MOUSE_EXTRA3 = 8,
};

enum GamepadButton : uint8_t {
  PAD_A = 0,
  PAD_B = 1,
  PAD_X = 2,
  PAD_Y = 3,
  PAD_LB = 4,
  PAD_RB = 5,
  PAD_LT = 6, // never sent as a button, analog on axis Z
  PAD_RT = 7, // never sent as a button, analog on axis RZ
  PAD_BACK = 8,
  PAD_START = 9,
  PAD_LSTICK = 10,
  PAD_RSTICK = 11,
};

enum GamepadAxis : uint8_t {
  AXIS_X = 0,
  AXIS_Y = 1,
  AXIS_Z = 2,
  AXIS_RX = 3,
  AXIS_RY = 4,
  AXIS_RZ = 5
};

// POV bitmask
constexpr uint32_t POV_CENTER = 0;
constexpr uint32_t POV_N = 1;
constexpr uint32_t POV_S = 16;
constexpr uint32_t POV_E = 256;
constexpr uint32_t POV_W = 4096;

// lock_keys bits
constexpr uint8_t LOCK_SCROLL = 1;
constexpr uint8_t LOCK_NUM = 2;
constexpr uint8_t LOCK_CAPS = 4;
} // namespace pk

// Clean-room implementation of the web player's InputTransmitter (protocol
// doc §8): records input events, keeps the ones the server hasn't confirmed,
// and serialises them into the binary V2 message layout. The output of
// flush() is the *uncompressed* RawMessage; the caller zlib-deflates it and
// wraps it in CS_INPUT_MESSAGE_V2. Not thread-safe.
class InputTransmitter {
public:
  enum class GamepadType : uint8_t { DInput = 0, XInput = 1, DS4 = 2, DS4Android = 3 };

  enum Kind : uint8_t {
    KEY_PRESS = 0x01,
    KEY_RELEASE = 0x02,
    MOUSE_MOVE = 0x03,
    MOUSE_SCROLL = 0x04,
    MOUSE_PRESS = 0x05,
    MOUSE_RELEASE = 0x06,
    GAMEPAD_PRESS = 0x07,
    GAMEPAD_RELEASE = 0x08,
    GAMEPAD_AXIS = 0x09,
    RELEASE_ALL = 0x0A,
    GAMEPAD_POV = 0x0B,
    LOCALE = 0x0C,
    TOUCH_DOWN = 0x0D,
    TOUCH_UP = 0x0E,
    TOUCH_MOVE = 0x0F,
  };

  static constexpr int MAX_EVENTS_PER_MESSAGE = 20;
  static constexpr int MAX_GAMEPADS = 4;

  InputTransmitter();

  // Monotonic microseconds; the same clock is used for SC_WEBRTC_INPUT_REPORT RTT (§7.5)
  static uint64_t now();
  // Replace the clock (tests); nullptr restores the monotonic clock
  static void setClockForTesting(uint64_t (*clock)());

  void setToken(uint32_t token) { token_ = token; }
  uint32_t token() const { return token_; }

  // Events are only recorded while collecting (§8.5)
  void setCollectEventsFlag(bool collect) { collecting_ = collect; }

  void sendKeyboardEvent(uint8_t key, bool pressed, uint8_t lockKeys);
  void sendMouseMove(int16_t dx, int16_t dy, int16_t absX, int16_t absY);
  void sendMouseScroll(int16_t dz);
  void sendMouseButton(uint8_t button, bool pressed);
  void sendGamepadButton(int index, GamepadType type, uint8_t button, bool pressed);
  void sendGamepadAxis(int index, GamepadType type, uint8_t axis, int32_t value);
  void sendGamepadPov(int index, GamepadType type, uint32_t value);
  void sendLocaleEvent(const std::string &locale);
  void sendTouchEvent(Kind kind, bool absolute, int16_t x, int16_t y);

  // §8.5: builds the messages to (re)send now. sendHeld=true first appends
  // the "release everything" events (§8.6). Empty result ⇒ nothing to send.
  std::vector<std::vector<uint8_t>> flush(bool sendHeld);

  // SC_INPUT_REPORT.last_event_id
  void setLastConfirmedEventId(uint32_t id);
  uint32_t lastEventId() const { return eventCounter_; }
  uint32_t lastConfirmedEventId() const { return lastConfirmedId_; }
  bool empty() const { return pending_.empty(); }
  size_t pendingCount() const { return pending_.size(); }

  // Resets counters and all state (inputs stopped)
  void clear();

  // Inspection helpers (used by the input channel and tests)
  bool isKeyPressed(uint8_t key) const { return keyBit(state_.keys, key); }
  bool anyHeld() const;

private:
  struct Gamepad {
    uint16_t buttons = 0;
    std::array<int32_t, 6> axes{};
    uint32_t pov = 0;
  };

  struct State {
    std::array<uint8_t, 30> keys{};
    uint16_t mouseButtons = 0;
    std::array<Gamepad, MAX_GAMEPADS> pads{};
    uint8_t touchDown = 0;
    std::array<char, 8> locale{};
    uint8_t lockKeys = 0;
    uint8_t deviceMask = 0;
  };

  struct Event {
    uint32_t id = 0;
    uint64_t timestamp = 0;
    uint8_t kind = 0; // full kind byte, incl. gamepad index/type bits
    std::vector<uint8_t> payload;
    State state; // device state as of this event
    bool sent = false;
  };

  struct HeldGamepadEntry {
    uint8_t pad;
    bool isPov;
    uint8_t button;
    GamepadType type;
  };

  static bool keyBit(const std::array<uint8_t, 30> &keys, uint8_t key) {
    return key < 240 && (keys[key / 8] >> (key % 8)) & 1;
  }
  static void setKeyBit(std::array<uint8_t, 30> &keys, uint8_t key, bool on) {
    if (key >= 240)
      return;
    if (on)
      keys[key / 8] |= static_cast<uint8_t>(1u << (key % 8));
    else
      keys[key / 8] &= static_cast<uint8_t>(~(1u << (key % 8)));
  }
  static uint8_t gamepadKind(Kind base, int index, GamepadType type) {
    return static_cast<uint8_t>((index << 6) | (static_cast<int>(type) << 4) | base);
  }

  void record(uint8_t kind, std::vector<uint8_t> payload, uint64_t timestamp);
  void appendReleaseAll();
  std::vector<uint8_t> encodeMessage(size_t first, size_t count);

  uint32_t token_ = 0;
  uint32_t messageCounter_ = 0;
  uint32_t eventCounter_ = 0;
  uint32_t lastConfirmedId_ = 0;
  bool collecting_ = false;
  uint64_t lastEventTimestamp_ = 0;

  State state_;
  std::deque<Event> pending_;

  // Press-order bookkeeping for "release everything" (§8.6)
  std::vector<uint8_t> heldKeys_;
  std::vector<uint8_t> heldMouseButtons_;
  std::vector<HeldGamepadEntry> heldGamepad_;
  std::vector<uint8_t> lastTouchDownPayload_;
};

} // namespace vkpcnx::stream
