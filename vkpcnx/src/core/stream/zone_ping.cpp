#include "core/stream/zone_ping.hpp"

#include <algorithm>
#include <borealis.hpp>

#include "core/utils/thread.hpp"
#include "core/utils/tls.hpp"

namespace vkpcnx::stream {

using Clock = std::chrono::steady_clock;

struct ZonePing::Probe {
  Server server;
  WebSocket ws;
  std::mutex mutex;
  uint32_t nextId = 0;
  uint32_t outstandingId = 0;
  bool outstanding = false;
  Clock::time_point sentAt;
  Clock::time_point lastSendAt;
  Clock::time_point startedAt = Clock::now();
  std::vector<double> rttMs;
  bool done = false;
  int32_t result = PING_FAILED;
  // Fired once, mutex held, when the probe has its result. Doesn't wait for
  // the socket to finish closing: that can take far longer than the ping.
  std::function<void()> onDone;

  static std::vector<uint8_t> encodeId(uint32_t id) {
    return {
      static_cast<uint8_t>(id),
      static_cast<uint8_t>(id >> 8),
      static_cast<uint8_t>(id >> 16),
      static_cast<uint8_t>(id >> 24)
    };
  }

  // mutex held
  void sendNext() {
    outstandingId = nextId++;
    outstanding = true;
    sentAt = lastSendAt = Clock::now();
    ws.sendBinary(encodeId(outstandingId));
  }

  // mutex held
  void finish(int32_t value) {
    if (done)
      return;
    done = true;
    result = value;
    ws.close(1000, "done");
    if (onDone)
      onDone();
  }

  void onEcho(const std::vector<uint8_t> &data) {
    std::lock_guard<std::mutex> lock(mutex);
    if (done || data.size() != 4)
      return;
    uint32_t id =
      data[0] | (data[1] << 8) | (data[2] << 16) | (static_cast<uint32_t>(data[3]) << 24);
    if (!outstanding || id != outstandingId)
      return;
    auto now = Clock::now();
    rttMs.push_back(std::chrono::duration<double, std::milli>(now - sentAt).count());
    outstanding = false;
    if (static_cast<int>(rttMs.size()) >= ECHO_COUNT) {
      double sum = 0;
      for (double v : rttMs)
        sum += v;
      double avg = sum / rttMs.size();
      finish(static_cast<int32_t>(avg * 1000.0)); // µs, truncated
      return;
    }
    sendNext();
  }

  // Called from the tick thread: re-send after 500 ms without an echo, give
  // up after the overall timeout.
  void tick() {
    std::lock_guard<std::mutex> lock(mutex);
    if (done)
      return;
    auto now = Clock::now();
    if (now - startedAt > std::chrono::milliseconds(TOTAL_TIMEOUT_MS)) {
      finish(PING_FAILED);
      return;
    }
    if (outstanding && ws.isOpen() &&
        now - lastSendAt >= std::chrono::milliseconds(RESEND_INTERVAL_MS)) {
      lastSendAt = now;
      ws.sendBinary(encodeId(outstandingId));
    }
  }
};

ZonePing::ZonePing() = default;

ZonePing::~ZonePing() {
  *alive_ = false;
  cancel();
}

void ZonePing::run(const std::vector<Server> &servers, Callback done) {
  cancel();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    done_ = std::move(done);
    finished_ = false;
    probes_.clear();
  }

  if (servers.empty()) {
    brls::sync([this, alive = alive_] {
      if (*alive)
        checkDone();
    });
    return;
  }

  for (const auto &server : servers) {
    auto probe = std::make_shared<Probe>();
    probe->server = server;
    std::weak_ptr<Probe> weak = probe;
    probe->onDone = [this, alive = alive_] {
      brls::sync([this, alive] {
        if (*alive)
          checkDone();
      });
    };

    probe->ws.onOpen = [weak] {
      if (auto p = weak.lock()) {
        std::lock_guard<std::mutex> lock(p->mutex);
        if (!p->done)
          p->sendNext();
      }
    };
    probe->ws.onBinary = [weak](const std::vector<uint8_t> &data) {
      if (auto p = weak.lock())
        p->onEcho(data);
    };
    probe->ws.onError = [weak](const std::string &err) {
      if (auto p = weak.lock()) {
        std::lock_guard<std::mutex> lock(p->mutex);
        brls::Logger::warning("ZonePing: {}:{} failed: {}", p->server.host, p->server.port, err);
        p->finish(PING_FAILED);
      }
    };
    probe->ws.onClose = [weak](int, const std::string &) {
      if (auto p = weak.lock()) {
        std::lock_guard<std::mutex> lock(p->mutex);
        p->finish(PING_FAILED); // no-op if it already has a result
      }
    };

    {
      std::lock_guard<std::mutex> lock(mutex_);
      probes_.push_back(probe);
    }

    WebSocket::Options opts;
    opts.connectTimeoutSecs = CONNECT_TIMEOUT_MS / 1000;
    opts.pingIntervalSecs = 0;
    opts.directCallbacks = true; // measure RTT on the socket thread, not per UI frame
    opts.caFilePath = vkpcnx::utils::caBundlePath();
    opts.verifyTls = (!opts.caFilePath.empty() || vkpcnx::utils::hasSystemCaStore()) &&
                     !vkpcnx::utils::allowInsecureTls();
    probe->ws.connect("wss://" + server.host + ":" + std::to_string(server.port), opts);
  }

  if (onProgress)
    onProgress(0, static_cast<int>(servers.size()));

  tick_.start(std::chrono::milliseconds(50), [this] {
    std::vector<std::shared_ptr<Probe>> probes;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      probes = probes_;
    }
    for (auto &p : probes)
      p->tick();
  });
}

void ZonePing::cancel() {
  tick_.stop();
  std::vector<std::shared_ptr<Probe>> probes;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    probes.swap(probes_);
    done_ = nullptr;
    finished_ = true;
  }
  for (auto &p : probes) {
    std::lock_guard<std::mutex> lock(p->mutex);
    p->finish(PING_FAILED);
  }
  // Destroying a probe joins its socket thread, which can block until the
  // socket finishes closing (tens of seconds on a bad connection): never on
  // the caller's thread, which is usually the UI thread.
  if (!probes.empty())
    vkpcnx::utils::runDetached([probes = std::move(probes)]() mutable { probes.clear(); });
}

void ZonePing::checkDone() {
  std::vector<Result> results;
  Callback done;
  int completed = 0;
  int total = 0;
  bool allDone = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (finished_)
      return;
    total = static_cast<int>(probes_.size());
    for (auto &p : probes_) {
      std::lock_guard<std::mutex> plock(p->mutex);
      if (p->done)
        completed++;
    }
    allDone = completed == total;
    if (allDone) {
      finished_ = true;
      for (auto &p : probes_) {
        std::lock_guard<std::mutex> plock(p->mutex);
        results.push_back({p->server.serverId, p->result, p->server.host});
      }
      done = std::move(done_);
      done_ = nullptr;
    }
  }

  if (onProgress)
    onProgress(completed, total);
  if (!allDone)
    return;

  tick_.stop();
  std::sort(results.begin(), results.end(), [](const Result &a, const Result &b) {
    return a.pingMicros < b.pingMicros;
  });
  for (auto &r : results)
    brls::Logger::info(
      "ZonePing: server {} ({}) -> {}",
      r.serverId,
      r.host,
      r.pingMicros == PING_FAILED ? "failed" : std::to_string(r.pingMicros / 1000.0) + " ms"
    );
  if (done)
    done(std::move(results));
}

} // namespace vkpcnx::stream
