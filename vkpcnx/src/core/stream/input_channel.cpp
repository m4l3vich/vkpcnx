#include "core/stream/input_channel.hpp"
#include "core/diag/log.hpp"

#include <algorithm>
#include <borealis/core/logger.hpp>
#include <chrono>
#include <cmath>
#include <cstring>
#include <zlib.h>

#include "client_server.pb.h"
#include "server_client.pb.h"

#include "core/utils/thread.hpp"

namespace vkpcnx::stream {

namespace proto = cg::network::protocol;
using proto::MessageType;

constexpr double InputRateLimiter::LEVELS[];

// ---- zlib helpers ------------------------------------------------------------

static std::string zlibDeflate(const std::vector<uint8_t> &raw) {
  uLongf outLen = compressBound(static_cast<uLong>(raw.size()));
  std::string out(outLen, '\0');
  if (compress2(
        reinterpret_cast<Bytef *>(out.data()),
        &outLen,
        raw.data(),
        static_cast<uLong>(raw.size()),
        Z_DEFAULT_COMPRESSION
      ) != Z_OK)
    return {};
  out.resize(outLen);
  return out;
}

static std::string zlibDeflate(const std::string &raw) {
  return zlibDeflate(std::vector<uint8_t>(raw.begin(), raw.end()));
}

static int inflateWith(
  const std::string &compressed, size_t rawSize, int windowBits, std::vector<uint8_t> &out
) {
  z_stream zs{};
  int rc = inflateInit2(&zs, windowBits);
  if (rc != Z_OK)
    return rc;
  out.assign(rawSize, 0);
  zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(compressed.data()));
  zs.avail_in = static_cast<uInt>(compressed.size());
  zs.next_out = out.data();
  zs.avail_out = static_cast<uInt>(out.size());
  rc = inflate(&zs, Z_FINISH);
  size_t produced = zs.total_out;
  inflateEnd(&zs);
  if (rc != Z_STREAM_END)
    return rc == Z_OK || rc == Z_BUF_ERROR ? Z_BUF_ERROR : rc;
  out.resize(produced);
  return Z_OK;
}

// Returns the zlib status; `out` holds the inflated data on Z_OK. Accepts zlib
// and gzip framing, then falls back to a raw deflate stream.
static int zlibInflateRc(const std::string &compressed, size_t rawSize, std::vector<uint8_t> &out) {
  int rc = inflateWith(compressed, rawSize, 15 + 32, out);
  if (rc == Z_DATA_ERROR && inflateWith(compressed, rawSize, -15, out) == Z_OK)
    return Z_OK;
  return rc;
}

static bool zlibInflate(const std::string &compressed, size_t rawSize, std::vector<uint8_t> &out) {
  return zlibInflateRc(compressed, rawSize, out) == Z_OK;
}

// ---- cursor decoding (§7.6) ---------------------------------------------------

static uint16_t rd16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
static int32_t rd32(const uint8_t *p) {
  return static_cast<int32_t>(
    p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24)
  );
}

bool decodeCursorBlob(const std::vector<uint8_t> &blob, CursorImage &out) {
  size_t hotspotOff, headerOff, pixelOff;
  if (blob.size() >= 138 && std::memcmp(blob.data(), "RIFF", 4) == 0 &&
      std::memcmp(blob.data() + 8, "ACON", 4) == 0) {
    hotspotOff = 86;
    headerOff = 98;
    pixelOff = 138;
  } else if (blob.size() >= 44) {
    hotspotOff = 0;
    headerOff = 4;
    pixelOff = 44;
  } else {
    return false;
  }
  const uint8_t *p = blob.data();
  out.hotspotX = rd16(p + hotspotOff);
  out.hotspotY = rd16(p + hotspotOff + 2);
  int32_t width = rd32(p + headerOff + 4);
  int32_t height2 = rd32(p + headerOff + 8);
  uint16_t bitCount = rd16(p + headerOff + 14);
  int height = std::abs(height2) / 2;
  if (width <= 0 || height <= 0 || width > 512 || height > 512)
    return false;
  if (bitCount != 32 && bitCount != 1)
    return false;

  out.width = width;
  out.height = height;
  out.rgba.assign(static_cast<size_t>(width) * height * 4, 0);

  size_t maskStride = ((static_cast<size_t>(width) + 31) / 32) * 4;
  auto maskBit = [&](size_t maskBase, int x, int y) -> bool {
    // bottom-up rows
    size_t row = static_cast<size_t>(height - 1 - y);
    size_t idx = maskBase + row * maskStride + x / 8;
    if (idx >= blob.size())
      return false;
    return (p[idx] >> (7 - (x % 8))) & 1;
  };

  if (bitCount == 32) {
    size_t xorStride = static_cast<size_t>(width) * 4;
    size_t andBase = pixelOff + xorStride * height;
    bool hasAlpha = false;
    for (int y = 0; y < height && !hasAlpha; y++)
      for (int x = 0; x < width; x++) {
        size_t idx = pixelOff + static_cast<size_t>(y) * xorStride + x * 4 + 3;
        if (idx < blob.size() && p[idx] != 0) {
          hasAlpha = true;
          break;
        }
      }
    for (int y = 0; y < height; y++) {
      size_t srcRow = pixelOff + static_cast<size_t>(height - 1 - y) * xorStride;
      for (int x = 0; x < width; x++) {
        size_t s = srcRow + x * 4;
        if (s + 3 >= blob.size())
          return false;
        uint8_t *d = &out.rgba[(static_cast<size_t>(y) * width + x) * 4];
        d[0] = p[s + 2];
        d[1] = p[s + 1];
        d[2] = p[s + 0];
        if (hasAlpha)
          d[3] = p[s + 3];
        else
          d[3] = (andBase < blob.size() && maskBit(andBase, x, y)) ? 0 : 255;
      }
    }
  } else {
    size_t xorBase = pixelOff;
    size_t andBase = pixelOff + maskStride * height;
    for (int y = 0; y < height; y++)
      for (int x = 0; x < width; x++) {
        bool xorBit = maskBit(xorBase, x, y);
        bool andBit = maskBit(andBase, x, y);
        uint8_t *d = &out.rgba[(static_cast<size_t>(y) * width + x) * 4];
        uint8_t v = xorBit ? 255 : 0;
        d[0] = d[1] = d[2] = v;
        d[3] = (andBit && !xorBit) ? 0 : 255; // AND=1,XOR=1 (invert) drawn as black
        if (andBit && xorBit)
          d[0] = d[1] = d[2] = 0;
      }
  }
  return true;
}

// ---- rate limiter (§7.10) -----------------------------------------------------

void InputRateLimiter::update(size_t bufferedAmount, uint64_t nowMs) {
  if (nowMs - startMs_ < 30000) {
    // first 30 s: start at 10 ms, only ever go up
    int target = levelFor(bufferedAmount);
    if (target > level_) {
      level_ = target;
      lastChangeMs_ = nowMs;
    }
    return;
  }
  int target = levelFor(bufferedAmount);
  if (target > level_) {
    level_ = target; // increase immediately, held ≥ 10 s
    lastChangeMs_ = nowMs;
  } else if (target < level_ && nowMs - lastChangeMs_ >= 10000) {
    level_--; // step down one level at most every 10 s
    lastChangeMs_ = nowMs;
  }
}

// ---- InputChannel --------------------------------------------------------------

InputChannel::InputChannel(WebRtcSession &rtc) : rtc_(rtc) {}

InputChannel::~InputChannel() { stop(); }

uint64_t InputChannel::nowMs() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
    duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count()
  );
}

void InputChannel::start(uint32_t token) {
  stop();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    token_ = token;
    tx_.clear();
    tx_.setToken(token);
    lastLocale_ = "unspecified";
    mouseSettingsId_ = 0;
    confirmedMouseSettingsId_ = 0;
    mouseSettingsRedundancy_.reset();
    inputRedundancy_.reset();
    clipboardRedundancy_.reset();
    rateLimiter_.reset(nowMs());
    pendingDx_ = pendingDy_ = 0;
    mouseButtonsDown_ = 0;
    padButtonsDown_.fill(0);
    padPov_.fill(0);
    for (auto &a : axisDirty_)
      a.fill(false);
    cursorTransfers_.clear();
    cursorImages_.clear();
    notifiedCursorId_ = UINT32_MAX;
    notifiedCursorShown_ = false;
    cursorId_ = cursorLast_ = 0;
    cursorReceivedIds_.clear();
    outgoingClipboardPackets_.clear();
    outgoingClipboardAcked_.clear();
    incomingClipboard_ = {};
    clipboardConfirmedId_ = 0;
    lastInitSentMs_ = 0;
  }
  started_ = false;
  running_ = true;
  thread_ = std::thread([this] {
    vkpcnx::diag::setThreadName("input");
    processLoop();
  });
}

void InputChannel::stop() {
  if (running_) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (started_ && rtc_.isDataChannelOpen())
        flushLocked(true); // release everything (§8.6)
    }
    running_ = false;
  }
  if (thread_.joinable()) {
    if (thread_.get_id() == std::this_thread::get_id())
      vkpcnx::utils::detach(std::move(thread_));
    else
      thread_.join();
  }
  started_ = false;
  // focused_ stays: it mirrors the stream screen's capture state, which
  // setFocus() only reports on change. Clearing it here dropped every event
  // when the channel (re)started after the screen had already captured:
  // the data channel opening after Streaming, or after a reconnect.
  std::lock_guard<std::mutex> lock(mutex_);
  tx_.clear();
}

void InputChannel::sendFrame(MessageType type, const std::string &payload) {
  // §7: drop any send while the channel is congested
  if (rtc_.dataChannelBufferedAmount() > MAX_BUFFERED_AMOUNT)
    return;
  auto bytes = encodeFrame(static_cast<uint32_t>(type), payload);
  rtc_.sendInput(bytes.data(), bytes.size());
}

void InputChannel::sendInit() {
  // §7.1: aes_message is just the token as uint32 LE (no AES involved)
  proto::cs::InputInitV2 init;
  std::string tokenBytes(4, '\0');
  tokenBytes[0] = static_cast<char>(token_);
  tokenBytes[1] = static_cast<char>(token_ >> 8);
  tokenBytes[2] = static_cast<char>(token_ >> 16);
  tokenBytes[3] = static_cast<char>(token_ >> 24);
  init.set_aes_message(tokenBytes);
  init.set_version(INPUT_PROTOCOL_VERSION);
  sendFrame(MessageType::CS_INPUT_INIT_V2, init.SerializeAsString());
}

void InputChannel::sendMouseSettingsLocked(uint64_t now) {
  if (windowWidth_ <= 0 || windowHeight_ <= 0)
    return;
  proto::cs::MouseSettings ms;
  ms.set_id(mouseSettingsId_);
  ms.set_token(token_);
  ms.set_sensitivity(1);
  ms.set_acceleration(10);
  ms.set_window_width(windowWidth_);
  ms.set_window_height(windowHeight_);
  ms.set_client_cursor(true);
  sendFrame(MessageType::CS_MOUSE_SETTINGS, ms.SerializeAsString());
  mouseSettingsRedundancy_.commitSent(mouseSettingsId_, now);
}

void InputChannel::sendInputReportLocked() {
  proto::cs::InputReport r;
  r.set_token(token_);
  r.set_cursor_id(cursorId_);
  r.set_cursor_last(cursorLast_);
  for (uint32_t id : cursorReceivedIds_)
    r.add_cursor_received_ids(id);
  r.set_clipboard_confirmed_id(clipboardConfirmedId_);
  r.set_clipboard_current_id(incomingClipboard_.id);
  for (const auto &[pid, _] : incomingClipboard_.packets)
    r.add_clipboard_current_received_ids(pid);
  sendFrame(MessageType::CS_INPUT_REPORT, r.SerializeAsString());
}

void InputChannel::flushLocked(bool sendHeld) {
  auto messages = tx_.flush(sendHeld);
  if (messages.empty())
    return;
  size_t buffered = rtc_.dataChannelBufferedAmount();
  inputRedundancy_.updateInterval(buffered);
  rateLimiter_.update(buffered, nowMs());
  for (const auto &raw : messages) {
    std::string deflated = zlibDeflate(raw);
    if (deflated.empty())
      continue;
    proto::cs::InputMessageV2 msg;
    msg.set_data(deflated);
    sendFrame(MessageType::CS_INPUT_MESSAGE_V2, msg.SerializeAsString());
  }
  inputRedundancy_.commitSent(tx_.lastEventId(), nowMs());
}

void InputChannel::sendLockSyncLocked() {
  // §9.1: press + release of key 0 carrying the current lock state
  tx_.setCollectEventsFlag(true);
  tx_.sendKeyboardEvent(pk::KEY_UNASSIGNED, true, lockKeys_);
  tx_.setCollectEventsFlag(false);
  flushLocked(false);
  tx_.setCollectEventsFlag(true);
  tx_.sendKeyboardEvent(pk::KEY_UNASSIGNED, false, lockKeys_);
  tx_.setCollectEventsFlag(false);
  flushLocked(false);
}

void InputChannel::processLoop() {
  while (running_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(PROCESS_INTERVAL_MS));
    if (!running_)
      break;
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t now = nowMs();

    if (!started_) {
      // §7.1: CS_INPUT_INIT_V2 every 100 ms until M_TOKEN_ACCEPTED
      if (now - lastInitSentMs_ >= static_cast<uint64_t>(INIT_RETRY_INTERVAL_MS) &&
          rtc_.isDataChannelOpen()) {
        lastInitSentMs_ = now;
        sendInit();
      }
      continue;
    }

    size_t buffered = rtc_.dataChannelBufferedAmount();
    rateLimiter_.update(buffered, now);

    // §7.8 1. mouse settings
    if (mouseSettingsId_ > confirmedMouseSettingsId_) {
      mouseSettingsRedundancy_.updateInterval(buffered);
      if (!mouseSettingsRedundancy_.isExtraRedundant(mouseSettingsId_, now))
        sendMouseSettingsLocked(now);
    }

    // §7.8 2. client clipboard
    resendClipboardLocked(now);

    // Rate-limited mouse move / gamepad axes
    if (focused_) {
      emitMouseMoveLocked(now);
      emitGamepadAxesLocked(now);
    }

    // §7.8 3. input redundancy
    if (!tx_.empty()) {
      inputRedundancy_.updateInterval(buffered);
      if (!inputRedundancy_.isExtraRedundant(tx_.lastEventId(), now))
        flushLocked(false);
    }
  }
}

// ---- server messages -----------------------------------------------------------

void InputChannel::onMessage(const rtc::binary &message) {
  auto frame = decodeFrame(reinterpret_cast<const uint8_t *>(message.data()), message.size());
  if (!frame) {
    brls::Logger::warning("InputChannel: malformed frame ({} bytes)", message.size());
    return;
  }

  switch (frame->type) {
  case MessageType::M_KEEP_ALIVE:
    return;
  case MessageType::M_ERROR: {
    proto::Error err;
    err.ParseFromString(frame->payload);
    brls::Logger::error(
      "InputChannel: M_ERROR {}: {}", static_cast<int>(err.error_type()), err.error_message()
    );
    if (onError)
      onError(err.user_message().empty() ? err.error_message() : err.user_message());
    return;
  }
  case MessageType::M_TOKEN_ACCEPTED: {
    if (started_)
      return;
    brls::Logger::info("InputChannel: token accepted, inputs started");
    {
      std::lock_guard<std::mutex> lock(mutex_);
      started_ = true;
      rateLimiter_.reset(nowMs());
      mouseSettingsId_ = 1;
      sendMouseSettingsLocked(nowMs());
      // Captured before the channel started: what setFocus(true) would have
      // sent on a started channel
      if (focused_)
        sendLockSyncLocked();
    }
    if (onTokenAccepted)
      onTokenAccepted();
    return;
  }
  default:
    break;
  }

  if (!started_) {
    brls::Logger::debug("InputChannel: {} before M_TOKEN_ACCEPTED", messageTypeName(frame->type));
    return;
  }

  switch (frame->type) {
  case MessageType::SC_INPUT_REPORT:
    handleInputReport(frame->payload);
    break;
  case MessageType::SC_WEBRTC_INPUT_REPORT:
    handleWebRtcInputReport(frame->payload);
    break;
  case MessageType::SC_CURSOR:
    handleCursor(frame->payload);
    break;
  case MessageType::SC_SEND_CLIPBOARD_DATA:
    handleClipboard(frame->payload);
    break;
  default:
    brls::Logger::debug("InputChannel: unhandled {}", messageTypeName(frame->type));
    break;
  }
}

void InputChannel::handleInputReport(const std::string &payload) {
  proto::sc::InputReport r;
  r.ParseFromString(payload);
  std::lock_guard<std::mutex> lock(mutex_);
  if (r.token() != token_)
    return;
  tx_.setLastConfirmedEventId(r.last_event_id());
  if (r.last_mouse_settings_id() > confirmedMouseSettingsId_)
    confirmedMouseSettingsId_ = r.last_mouse_settings_id();
  // Clipboard acks (§7.7)
  if (!outgoingClipboardPackets_.empty()) {
    if (r.clipboard_confirmed_id() >= outgoingClipboard_.id) {
      outgoingClipboardPackets_.clear();
      outgoingClipboardAcked_.clear();
    } else if (r.clipboard_current_id() == outgoingClipboard_.id) {
      for (uint32_t pid : r.clipboard_current_received_ids())
        if (pid < outgoingClipboardAcked_.size())
          outgoingClipboardAcked_[pid] = true;
    }
  }
}

void InputChannel::handleWebRtcInputReport(const std::string &payload) {
  proto::sc::WebRTCInputReport r;
  r.ParseFromString(payload);
  if (r.token() != token_)
    return;
  // §7.5
  double rtt =
    (static_cast<double>(InputTransmitter::now()) - static_cast<double>(r.sent_input_timestamp())) /
      1000.0 -
    static_cast<double>(r.report_delay_on_server_ms());
  if (rtt < 0)
    return;
  lastRttMs_ = rtt;
  if (onInputRtt)
    onInputRtt(rtt);
}

void InputChannel::handleCursor(const std::string &payload) {
  proto::sc::Cursor c;
  c.ParseFromString(payload);
  CursorImage decoded;
  bool notify = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (c.token() != token_)
      return;
    cursorLast_ = std::max(cursorLast_, c.last());
    bool current = c.last() >= cursorLast_;

    if (c.raw_size() == 0) {
      // A reference to an image sent earlier under this id (the server
      // re-selects cached cursors this way, e.g. arrow <-> hand); an id we
      // have no image for means "no cursor"
      if (current) {
        cursorId_ = c.id();
        cursorReceivedIds_ = {c.packet_id()};
        auto it = cursorImages_.find(c.id());
        if (it != cursorImages_.end())
          decoded = it->second;
        decoded.id = c.id();
        bool shown = !decoded.rgba.empty();
        notify = c.id() != notifiedCursorId_ || shown != notifiedCursorShown_;
      }
    } else {
      auto &t = cursorTransfers_[c.id()];
      if (t.rawSize != c.raw_size() || t.packets != c.packets())
        t.received.clear(); // a different image reusing the id
      t.id = c.id();
      t.packets = c.packets();
      t.last = std::max(t.last, c.last());
      t.rawSize = c.raw_size();
      t.received[c.packet_id()] = c.payload();

      cursorReceivedIds_.clear();
      for (const auto &[pid, _] : t.received)
        cursorReceivedIds_.push_back(pid);

      if (t.packets > 0 && t.received.size() >= t.packets) {
        std::string compressed;
        for (uint32_t i = 0; i < t.packets; i++)
          compressed += t.received[i];
        std::vector<uint8_t> blob;
        int rc = zlibInflateRc(compressed, t.rawSize, blob);
        if (rc == Z_OK && decodeCursorBlob(blob, decoded)) {
          decoded.id = c.id();
          cursorImages_[c.id()] = decoded;
          if (cursorImages_.size() > 64) // keep the cache bounded
            cursorImages_.erase(cursorImages_.begin());
          if (current) {
            cursorId_ = c.id();
            notify = true;
          }
        } else {
          std::string ids;
          for (const auto &[pid, data] : t.received)
            ids += fmt::format("{}{}:{}", ids.empty() ? "" : " ", pid, data.size());
          auto hex = [](const auto &bytes) {
            std::string h;
            for (size_t i = 0; i < std::min<size_t>(bytes.size(), 16); i++)
              h += fmt::format("{:02x}", static_cast<uint8_t>(bytes[i]));
            return h;
          };
          brls::Logger::warning(
            "InputChannel: cannot decode cursor {} (raw {} bytes, {} packet(s) [{}], "
            "compressed {} bytes {}, inflate rc {}{})",
            c.id(),
            t.rawSize,
            t.packets,
            ids,
            compressed.size(),
            hex(compressed),
            rc,
            rc == Z_OK ? ", header rejected: " + hex(blob) : ""
          );
        }
        cursorTransfers_.erase(c.id()); // done; the image lives in cursorImages_
      }
    }
    if (notify) {
      notifiedCursorId_ = decoded.id;
      notifiedCursorShown_ = !decoded.rgba.empty();
    }
    sendInputReportLocked(); // §7.4: reply for every packet
  }
  if (notify && onCursor)
    onCursor(decoded);
}

void InputChannel::handleClipboard(const std::string &payload) {
  proto::sc::ClipboardData d;
  d.ParseFromString(payload);
  std::string text;
  bool complete = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (d.token() != token_)
      return;
    if (d.id() > clipboardConfirmedId_ && d.id() >= incomingClipboard_.id) {
      if (d.id() != incomingClipboard_.id) {
        incomingClipboard_ = {};
        incomingClipboard_.id = d.id();
      }
      incomingClipboard_.packetsCount = d.packets_count();
      incomingClipboard_.rawSize = d.raw_size();
      incomingClipboard_.packets[d.packet_id()] = d.content();
      if (incomingClipboard_.packetsCount > 0 &&
          incomingClipboard_.packets.size() >= incomingClipboard_.packetsCount) {
        std::string compressed;
        for (uint32_t i = 0; i < incomingClipboard_.packetsCount; i++)
          compressed += incomingClipboard_.packets[i];
        std::vector<uint8_t> raw;
        if (zlibInflate(compressed, incomingClipboard_.rawSize, raw)) {
          text.assign(raw.begin(), raw.end());
          complete = true;
        }
        clipboardConfirmedId_ = incomingClipboard_.id;
      }
    }
    sendInputReportLocked();
  }
  if (complete && onClipboardText)
    onClipboardText(text);
}

// ---- clipboard client → server --------------------------------------------------

void InputChannel::sendClipboardText(const std::string &text) {
  std::string deflated = zlibDeflate(text);
  std::lock_guard<std::mutex> lock(mutex_);
  if (!started_)
    return;
  outgoingClipboard_ = {};
  outgoingClipboard_.id = ++clipboardCounter_;
  outgoingClipboard_.rawSize = static_cast<uint32_t>(text.size());
  outgoingClipboardPackets_.clear();
  for (size_t off = 0; off < deflated.size(); off += CLIPBOARD_CHUNK)
    outgoingClipboardPackets_.push_back(deflated.substr(off, CLIPBOARD_CHUNK));
  if (outgoingClipboardPackets_.empty())
    outgoingClipboardPackets_.push_back("");
  outgoingClipboard_.packetsCount = static_cast<uint32_t>(outgoingClipboardPackets_.size());
  outgoingClipboardAcked_.assign(outgoingClipboardPackets_.size(), false);
  clipboardRedundancy_.reset();
  resendClipboardLocked(nowMs());
}

void InputChannel::resendClipboardLocked(uint64_t now) {
  if (outgoingClipboardPackets_.empty())
    return;
  clipboardRedundancy_.updateInterval(rtc_.dataChannelBufferedAmount());
  if (clipboardRedundancy_.isExtraRedundant(outgoingClipboard_.id, now))
    return;
  for (size_t i = 0; i < outgoingClipboardPackets_.size(); i++) {
    if (outgoingClipboardAcked_[i])
      continue;
    proto::cs::ClipboardData d;
    d.set_token(token_);
    d.set_id(outgoingClipboard_.id);
    d.set_packets_count(outgoingClipboard_.packetsCount);
    d.set_packet_id(static_cast<uint32_t>(i));
    d.set_raw_size(outgoingClipboard_.rawSize);
    d.set_content(outgoingClipboardPackets_[i]);
    sendFrame(MessageType::CS_SEND_CLIPBOARD_DATA, d.SerializeAsString());
  }
  clipboardRedundancy_.commitSent(outgoingClipboard_.id, now);
}

// ---- client state --------------------------------------------------------------

void InputChannel::setWindowSize(int width, int height) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (width == windowWidth_ && height == windowHeight_)
    return;
  // Rescale the virtual cursor proportionally (§9.2)
  if (windowWidth_ > 0 && windowHeight_ > 0) {
    absX_ = absX_ * width / windowWidth_;
    absY_ = absY_ * height / windowHeight_;
  } else {
    absX_ = width / 2.0;
    absY_ = height / 2.0;
  }
  windowWidth_ = width;
  windowHeight_ = height;
  if (started_) {
    mouseSettingsId_++;
    sendMouseSettingsLocked(nowMs());
  }
}

void InputChannel::setLockKeys(uint8_t lockKeys) {
  std::lock_guard<std::mutex> lock(mutex_);
  lockKeys_ = lockKeys;
}

void InputChannel::setFocus(bool focused) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (focused == focused_)
    return;
  focused_ = focused;
  if (!started_)
    return;
  if (focused) {
    sendLockSyncLocked();
  } else {
    pendingDx_ = pendingDy_ = 0;
    mouseButtonsDown_ = 0;
    padButtonsDown_.fill(0);
    padPov_.fill(0);
    flushLocked(true);
  }
}

void InputChannel::releaseAll() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!started_)
    return;
  mouseButtonsDown_ = 0;
  padButtonsDown_.fill(0);
  padPov_.fill(0);
  flushLocked(true);
}

void InputChannel::keyEvent(uint8_t dik, bool pressed, const std::string &locale) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!started_ || !focused_)
    return;
  // §9.1: auto-repeat / duplicate transitions are suppressed
  if (pressed == tx_.isKeyPressed(dik) && dik != pk::KEY_UNASSIGNED)
    return;
  if (!locale.empty() && locale != lastLocale_) {
    lastLocale_ = locale;
    tx_.setCollectEventsFlag(true);
    tx_.sendLocaleEvent(locale);
    tx_.setCollectEventsFlag(false);
    flushLocked(false);
  }
  uint8_t lockState = lockKeys_;
  // The server sees the *new* lock state on keydown of a lock key
  if (pressed) {
    if (dik == 58)
      lockState ^= pk::LOCK_CAPS;
    else if (dik == 69)
      lockState ^= pk::LOCK_NUM;
    else if (dik == 70)
      lockState ^= pk::LOCK_SCROLL;
  }
  tx_.setCollectEventsFlag(true);
  tx_.sendKeyboardEvent(dik, pressed, lockState);
  tx_.setCollectEventsFlag(false);
  flushLocked(false);
}

void InputChannel::mouseMove(int dx, int dy) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!started_ || !focused_)
    return;
  pendingDx_ += dx;
  pendingDy_ += dy;
  absX_ = std::clamp(absX_ + dx, 0.0, static_cast<double>(std::max(windowWidth_, 0)));
  absY_ = std::clamp(absY_ + dy, 0.0, static_cast<double>(std::max(windowHeight_, 0)));
  uint64_t now = nowMs();
  if (now - lastMoveEmitMs_ >= static_cast<uint64_t>(rateLimiter_.intervalMs()))
    emitMouseMoveLocked(now);
}

void InputChannel::mouseMoveTo(int x, int y) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!started_ || !focused_)
    return;
  double nx =
    std::clamp(static_cast<double>(x), 0.0, static_cast<double>(std::max(windowWidth_, 0)));
  double ny =
    std::clamp(static_cast<double>(y), 0.0, static_cast<double>(std::max(windowHeight_, 0)));
  pendingDx_ += static_cast<int>(std::lround(nx - absX_));
  pendingDy_ += static_cast<int>(std::lround(ny - absY_));
  absX_ = nx;
  absY_ = ny;
  uint64_t now = nowMs();
  if (now - lastMoveEmitMs_ >= static_cast<uint64_t>(rateLimiter_.intervalMs()))
    emitMouseMoveLocked(now);
}

void InputChannel::emitMouseMoveLocked(uint64_t now) {
  if (pendingDx_ == 0 && pendingDy_ == 0)
    return;
  if (now - lastMoveEmitMs_ < static_cast<uint64_t>(rateLimiter_.intervalMs()))
    return;
  lastMoveEmitMs_ = now;
  tx_.setCollectEventsFlag(true);
  tx_.sendMouseMove(
    static_cast<int16_t>(pendingDx_),
    static_cast<int16_t>(pendingDy_),
    static_cast<int16_t>(absX_),
    static_cast<int16_t>(absY_)
  );
  tx_.setCollectEventsFlag(false);
  pendingDx_ = pendingDy_ = 0;
  flushLocked(false);
}

void InputChannel::mouseButton(uint8_t button, bool pressed) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!started_ || !focused_ || button >= 16)
    return;
  bool down = (mouseButtonsDown_ >> button) & 1;
  if (down == pressed)
    return; // duplicate filtered (§9.2)
  if (pressed)
    mouseButtonsDown_ |= static_cast<uint16_t>(1u << button);
  else
    mouseButtonsDown_ &= static_cast<uint16_t>(~(1u << button));
  tx_.setCollectEventsFlag(true);
  tx_.sendMouseButton(button, pressed);
  tx_.setCollectEventsFlag(false);
  flushLocked(false);
}

void InputChannel::mouseWheel(int steps) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!started_ || !focused_ || steps == 0)
    return;
  int sign = steps > 0 ? 1 : -1;
  tx_.setCollectEventsFlag(true);
  tx_.sendMouseScroll(static_cast<int16_t>(sign * 120));
  tx_.setCollectEventsFlag(false);
  flushLocked(false);
}

void InputChannel::gamepadButton(int index, uint8_t button, bool pressed) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!started_ || !focused_ || index < 0 || index >= InputTransmitter::MAX_GAMEPADS ||
      button >= 16)
    return;
  if (button == pk::PAD_LT || button == pk::PAD_RT)
    return; // §9.3: triggers are analog only
  bool down = (padButtonsDown_[index] >> button) & 1;
  if (down == pressed)
    return;
  if (pressed)
    padButtonsDown_[index] |= static_cast<uint16_t>(1u << button);
  else
    padButtonsDown_[index] &= static_cast<uint16_t>(~(1u << button));
  tx_.setCollectEventsFlag(true);
  tx_.sendGamepadButton(index, InputTransmitter::GamepadType::XInput, button, pressed);
  tx_.setCollectEventsFlag(false);
  flushLocked(false);
}

void InputChannel::gamepadAxis(int index, uint8_t axis, int32_t value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!started_ || index < 0 || index >= InputTransmitter::MAX_GAMEPADS || axis >= 6)
    return;
  if (axisValues_[index][axis] == value)
    return;
  axisValues_[index][axis] = value;
  axisDirty_[index][axis] = true;
}

void InputChannel::emitGamepadAxesLocked(uint64_t now) {
  if (now - lastAxisEmitMs_ < static_cast<uint64_t>(rateLimiter_.intervalMs()))
    return;
  bool any = false;
  for (int pad = 0; pad < InputTransmitter::MAX_GAMEPADS; pad++)
    for (int axis = 0; axis < 6; axis++)
      if (axisDirty_[pad][axis]) {
        axisDirty_[pad][axis] = false;
        tx_.setCollectEventsFlag(true);
        tx_.sendGamepadAxis(
          pad,
          InputTransmitter::GamepadType::XInput,
          static_cast<uint8_t>(axis),
          axisValues_[pad][axis]
        );
        tx_.setCollectEventsFlag(false);
        flushLocked(false); // one message per event (§8.5)
        any = true;
      }
  if (any)
    lastAxisEmitMs_ = now;
}

void InputChannel::gamepadPov(int index, uint32_t pov) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!started_ || !focused_ || index < 0 || index >= InputTransmitter::MAX_GAMEPADS)
    return;
  if (padPov_[index] == pov)
    return;
  padPov_[index] = pov;
  tx_.setCollectEventsFlag(true);
  tx_.sendGamepadPov(index, InputTransmitter::GamepadType::XInput, pov);
  tx_.setCollectEventsFlag(false);
  flushLocked(false);
}

} // namespace vkpcnx::stream
