#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "core/utils/thread.hpp"

namespace vkpcnx::utils {

// Runs a callback every `period` on a dedicated thread, independent of the UI
// frame loop (borealis timers only tick while frames are rendered). stop()
// blocks until the callback isn't running anymore, so it's safe to destroy
// whatever the callback touches right after.
class PeriodicTimer {
public:
  PeriodicTimer() = default;
  ~PeriodicTimer() { stop(); }
  PeriodicTimer(const PeriodicTimer &) = delete;
  PeriodicTimer &operator=(const PeriodicTimer &) = delete;

  void start(std::chrono::milliseconds period, std::function<void()> fn) {
    stop();
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = false;
    thread_ = std::thread([this, period, fn = std::move(fn)] {
      std::unique_lock<std::mutex> lock(mutex_);
      while (!stop_) {
        if (cv_.wait_for(lock, period, [this] { return stop_; }))
          break;
        lock.unlock();
        fn();
        lock.lock();
      }
    });
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
      if (thread_.get_id() == std::this_thread::get_id())
        detach(std::move(thread_)); // stop() from inside the callback
      else
        thread_.join();
    }
  }

  bool running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return thread_.joinable() && !stop_;
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::thread thread_;
  bool stop_ = true;
};

} // namespace vkpcnx::utils
