#include "core/stream/input_encoder.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace vkpcnx::stream {

namespace {

void putU8(std::vector<uint8_t> &out, uint8_t v) { out.push_back(v); }
void putU16(std::vector<uint8_t> &out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
}
void putU32(std::vector<uint8_t> &out, uint32_t v) {
  putU16(out, static_cast<uint16_t>(v));
  putU16(out, static_cast<uint16_t>(v >> 16));
}
void putU64(std::vector<uint8_t> &out, uint64_t v) {
  putU32(out, static_cast<uint32_t>(v));
  putU32(out, static_cast<uint32_t>(v >> 32));
}
void putI16(std::vector<uint8_t> &out, int16_t v) { putU16(out, static_cast<uint16_t>(v)); }
void putI32(std::vector<uint8_t> &out, int32_t v) { putU32(out, static_cast<uint32_t>(v)); }

} // namespace

InputTransmitter::InputTransmitter() = default;

static uint64_t (*g_clockOverride)() = nullptr;

void InputTransmitter::setClockForTesting(uint64_t (*clock)()) { g_clockOverride = clock; }

uint64_t InputTransmitter::now() {
  if (g_clockOverride)
    return g_clockOverride();
  using namespace std::chrono;
  return static_cast<uint64_t>(
    duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count()
  );
}

void InputTransmitter::clear() {
  messageCounter_ = 0;
  eventCounter_ = 0;
  lastConfirmedId_ = 0;
  lastEventTimestamp_ = 0;
  state_ = State{};
  pending_.clear();
  heldKeys_.clear();
  heldMouseButtons_.clear();
  heldGamepad_.clear();
  lastTouchDownPayload_.clear();
}

bool InputTransmitter::anyHeld() const {
  return !heldKeys_.empty() || !heldMouseButtons_.empty() || !heldGamepad_.empty() ||
         state_.touchDown;
}

void InputTransmitter::record(uint8_t kind, std::vector<uint8_t> payload, uint64_t timestamp) {
  Event ev;
  ev.id = ++eventCounter_;
  ev.timestamp = timestamp;
  ev.kind = kind;
  ev.payload = std::move(payload);
  ev.state = state_;
  pending_.push_back(std::move(ev));
}

void InputTransmitter::sendKeyboardEvent(uint8_t key, bool pressed, uint8_t lockKeys) {
  if (!collecting_)
    return;
  state_.deviceMask |= 0x01;
  state_.lockKeys = lockKeys;
  setKeyBit(state_.keys, key, pressed);
  if (pressed) {
    heldKeys_.push_back(key);
  } else {
    auto it = std::find(heldKeys_.begin(), heldKeys_.end(), key);
    if (it != heldKeys_.end())
      heldKeys_.erase(it);
  }
  uint64_t ts = now();
  lastEventTimestamp_ = ts;
  record(pressed ? KEY_PRESS : KEY_RELEASE, {key, lockKeys}, ts);
}

void InputTransmitter::sendMouseMove(int16_t dx, int16_t dy, int16_t absX, int16_t absY) {
  if (!collecting_)
    return;
  state_.deviceMask |= 0x02;
  std::vector<uint8_t> p;
  putI16(p, dx);
  putI16(p, dy);
  putI16(p, absX);
  putI16(p, absY);
  uint64_t ts = now();
  lastEventTimestamp_ = ts;
  record(MOUSE_MOVE, std::move(p), ts);
}

void InputTransmitter::sendMouseScroll(int16_t dz) {
  if (!collecting_)
    return;
  state_.deviceMask |= 0x02;
  std::vector<uint8_t> p;
  putI16(p, dz);
  uint64_t ts = now();
  lastEventTimestamp_ = ts;
  record(MOUSE_SCROLL, std::move(p), ts);
}

void InputTransmitter::sendMouseButton(uint8_t button, bool pressed) {
  if (!collecting_)
    return;
  state_.deviceMask |= 0x02;
  if (button < 16) {
    if (pressed)
      state_.mouseButtons |= static_cast<uint16_t>(1u << button);
    else
      state_.mouseButtons &= static_cast<uint16_t>(~(1u << button));
  }
  if (pressed) {
    heldMouseButtons_.push_back(button);
  } else {
    auto it = std::find(heldMouseButtons_.begin(), heldMouseButtons_.end(), button);
    if (it != heldMouseButtons_.end())
      heldMouseButtons_.erase(it);
  }
  uint64_t ts = now();
  lastEventTimestamp_ = ts;
  record(pressed ? MOUSE_PRESS : MOUSE_RELEASE, {button}, ts);
}

void InputTransmitter::sendGamepadButton(
  int index, GamepadType type, uint8_t button, bool pressed
) {
  if (!collecting_ || index < 0 || index >= MAX_GAMEPADS || button >= 16)
    return;
  state_.deviceMask |= static_cast<uint8_t>(0x04 << index);
  auto &pad = state_.pads[index];
  if (pressed)
    pad.buttons |= static_cast<uint16_t>(1u << button);
  else
    pad.buttons &= static_cast<uint16_t>(~(1u << button));
  if (pressed) {
    heldGamepad_.push_back({static_cast<uint8_t>(index), false, button, type});
  } else {
    auto it =
      std::find_if(heldGamepad_.begin(), heldGamepad_.end(), [&](const HeldGamepadEntry &e) {
        return !e.isPov && e.pad == index && e.button == button;
      });
    if (it != heldGamepad_.end())
      heldGamepad_.erase(it);
  }
  uint64_t ts = now();
  lastEventTimestamp_ = ts;
  record(gamepadKind(pressed ? GAMEPAD_PRESS : GAMEPAD_RELEASE, index, type), {button}, ts);
}

void InputTransmitter::sendGamepadAxis(int index, GamepadType type, uint8_t axis, int32_t value) {
  if (!collecting_ || index < 0 || index >= MAX_GAMEPADS || axis >= 6)
    return;
  state_.deviceMask |= static_cast<uint8_t>(0x04 << index);
  state_.pads[index].axes[axis] = value;
  std::vector<uint8_t> p;
  putU8(p, axis);
  putI32(p, value);
  uint64_t ts = now();
  lastEventTimestamp_ = ts;
  record(gamepadKind(GAMEPAD_AXIS, index, type), std::move(p), ts);
}

void InputTransmitter::sendGamepadPov(int index, GamepadType type, uint32_t value) {
  if (!collecting_ || index < 0 || index >= MAX_GAMEPADS)
    return;
  state_.deviceMask |= static_cast<uint8_t>(0x04 << index);
  auto &pad = state_.pads[index];
  bool wasCenter = pad.pov == pk::POV_CENTER;
  pad.pov = value;
  if (value != pk::POV_CENTER) {
    if (wasCenter)
      heldGamepad_.push_back({static_cast<uint8_t>(index), true, 0, type});
  } else {
    auto it =
      std::find_if(heldGamepad_.begin(), heldGamepad_.end(), [&](const HeldGamepadEntry &e) {
        return e.isPov && e.pad == index;
      });
    if (it != heldGamepad_.end())
      heldGamepad_.erase(it);
  }
  std::vector<uint8_t> p;
  putU8(p, 0); // pov id
  putU32(p, value);
  uint64_t ts = now();
  lastEventTimestamp_ = ts;
  record(gamepadKind(GAMEPAD_POV, index, type), std::move(p), ts);
}

void InputTransmitter::sendLocaleEvent(const std::string &locale) {
  if (!collecting_)
    return;
  state_.locale.fill(0);
  std::memcpy(state_.locale.data(), locale.data(), std::min<size_t>(locale.size(), 8));
  std::vector<uint8_t> p(state_.locale.begin(), state_.locale.end());
  // §8.2: timestamp of the previous event (0 initially)
  record(LOCALE, std::move(p), lastEventTimestamp_);
}

void InputTransmitter::sendTouchEvent(Kind kind, bool absolute, int16_t x, int16_t y) {
  if (!collecting_)
    return;
  if (kind != TOUCH_DOWN && kind != TOUCH_UP && kind != TOUCH_MOVE)
    return;
  state_.deviceMask |= 0x40;
  std::vector<uint8_t> p;
  putU8(p, absolute ? 1 : 0);
  putU8(p, 0);
  putI16(p, x);
  putI16(p, y);
  if (kind == TOUCH_DOWN || kind == TOUCH_MOVE) {
    state_.touchDown = 1;
    if (kind == TOUCH_DOWN)
      lastTouchDownPayload_ = p;
  } else {
    state_.touchDown = 0;
  }
  uint64_t ts = now();
  lastEventTimestamp_ = ts;
  record(kind, std::move(p), ts);
}

void InputTransmitter::appendReleaseAll() {
  uint64_t ts = now();
  lastEventTimestamp_ = ts;

  // 1. keys in press order
  for (uint8_t key : heldKeys_) {
    setKeyBit(state_.keys, key, false);
    record(KEY_RELEASE, {key, state_.lockKeys}, ts);
  }
  heldKeys_.clear();
  state_.keys.fill(0);

  // 2. mouse buttons in press order
  for (uint8_t button : heldMouseButtons_) {
    if (button < 16)
      state_.mouseButtons &= static_cast<uint16_t>(~(1u << button));
    record(MOUSE_RELEASE, {button}, ts);
  }
  heldMouseButtons_.clear();
  state_.mouseButtons = 0;

  // 3. gamepads, one chronological list across pads (axes are kept)
  for (const auto &e : heldGamepad_) {
    if (e.isPov) {
      state_.pads[e.pad].pov = 0;
      std::vector<uint8_t> p;
      putU8(p, 0);
      putU32(p, 0);
      record(gamepadKind(GAMEPAD_POV, e.pad, e.type), std::move(p), ts);
    } else {
      state_.pads[e.pad].buttons &= static_cast<uint16_t>(~(1u << e.button));
      record(gamepadKind(GAMEPAD_RELEASE, e.pad, e.type), {e.button}, ts);
    }
  }
  heldGamepad_.clear();
  for (auto &pad : state_.pads) {
    pad.buttons = 0;
    pad.pov = 0;
  }

  // 4. marker
  record(RELEASE_ALL, {}, ts);

  // 5. touch
  if (state_.touchDown) {
    state_.touchDown = 0;
    record(TOUCH_UP, lastTouchDownPayload_, ts);
  }
}

std::vector<uint8_t> InputTransmitter::encodeMessage(size_t first, size_t count) {
  const Event &newest = pending_[first + count - 1];
  const State &st = newest.state;

  std::vector<uint8_t> out;
  out.reserve(64 + count * 16);
  putU32(out, ++messageCounter_);
  putU32(out, token_);
  out.insert(out.end(), st.locale.begin(), st.locale.end());
  putU8(out, st.lockKeys);
  putU8(out, st.deviceMask);
  if (st.deviceMask & 0x01)
    out.insert(out.end(), st.keys.begin(), st.keys.end());
  if (st.deviceMask & 0x02)
    putU16(out, st.mouseButtons);
  for (int i = 0; i < MAX_GAMEPADS; i++) {
    if (!(st.deviceMask & (0x04 << i)))
      continue;
    const Gamepad &pad = st.pads[i];
    putU16(out, pad.buttons);
    for (int32_t axis : pad.axes)
      putI32(out, axis);
    putU8(out, static_cast<uint8_t>(pad.pov & 0xFF));
  }
  if (st.deviceMask & 0x40)
    putU8(out, st.touchDown);

  putU8(out, static_cast<uint8_t>(count));
  putU32(out, newest.id);
  putU64(out, newest.timestamp);

  // Events newest first
  for (size_t i = 0; i < count; i++) {
    const Event &ev = pending_[first + count - 1 - i];
    int64_t delta = static_cast<int64_t>(newest.timestamp) - static_cast<int64_t>(ev.timestamp);
    putI32(out, static_cast<int32_t>(delta));
    putU8(out, ev.kind);
    out.insert(out.end(), ev.payload.begin(), ev.payload.end());
  }
  return out;
}

std::vector<std::vector<uint8_t>> InputTransmitter::flush(bool sendHeld) {
  if (sendHeld)
    appendReleaseAll();

  std::vector<std::vector<uint8_t>> messages;
  size_t total = pending_.size();
  if (total == 0)
    return messages;

  // Index of the first event that has never been sent
  size_t p = 0;
  while (p < total && pending_[p].sent)
    p++;

  // Full chunks of new events, oldest first
  while (total - p > static_cast<size_t>(MAX_EVENTS_PER_MESSAGE)) {
    messages.push_back(encodeMessage(p, MAX_EVENTS_PER_MESSAGE));
    p += MAX_EVENTS_PER_MESSAGE;
  }
  // Always the newest ≤20 events, even if nothing is new (redundancy)
  size_t count = std::min<size_t>(MAX_EVENTS_PER_MESSAGE, total);
  messages.push_back(encodeMessage(total - count, count));

  for (auto &ev : pending_)
    ev.sent = true;
  return messages;
}

void InputTransmitter::setLastConfirmedEventId(uint32_t id) {
  if (id <= lastConfirmedId_)
    return;
  lastConfirmedId_ = id;
  while (!pending_.empty() && pending_.front().id <= id)
    pending_.pop_front();
}

} // namespace vkpcnx::stream
