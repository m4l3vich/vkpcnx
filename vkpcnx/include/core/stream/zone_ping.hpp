#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/utils/periodic_timer.hpp"
#include "core/websocket.hpp"

namespace vkpcnx::stream {

// Zone-server ping test (protocol doc §4.1): for every server open a
// WebSocket, exchange 4-byte LE message ids (the server echoes them), and
// report the average RTT in microseconds after 10 echoes. Failures and
// timeouts report PING_FAILED. Results are sorted ascending by ping.
class ZonePing {
public:
  static constexpr int32_t PING_FAILED = 2147483647;
  static constexpr int ECHO_COUNT = 10;
  static constexpr int CONNECT_TIMEOUT_MS = 5000;
  static constexpr int RESEND_INTERVAL_MS = 500;
  static constexpr int TOTAL_TIMEOUT_MS = 15000;

  struct Server {
    std::string host; // as it should appear in wss://host:port
    int port = 0;
    int64_t serverId = 0;
  };

  struct Result {
    int64_t serverId = 0;
    int32_t pingMicros = PING_FAILED;
    std::string host;
  };

  using Callback = std::function<void(std::vector<Result>)>;
  using ProgressCallback = std::function<void(int completed, int total)>;

  ZonePing();
  ~ZonePing();

  // Fired once with (0, total) when `run` starts, then again after every
  // server responds (or fails/times out), up to (total, total). Runs on the
  // UI thread, like `done`.
  ProgressCallback onProgress;

  // Runs all pings in parallel; `done` is invoked on the UI thread.
  void run(const std::vector<Server> &servers, Callback done);
  void cancel();

private:
  struct Probe;
  void checkDone();

  std::mutex mutex_;
  std::vector<std::shared_ptr<Probe>> probes_;
  Callback done_;
  bool finished_ = false;
  vkpcnx::utils::PeriodicTimer tick_;
  // Guards checkDone() hops queued with brls::sync against a destroyed ZonePing
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace vkpcnx::stream
