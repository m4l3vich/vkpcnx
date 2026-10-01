#include "core/utils/thread.hpp"

#ifdef __SWITCH__
#include <condition_variable>
#include <list>
#include <memory>
#include <mutex>
#endif

namespace vkpcnx::utils {

#ifdef __SWITCH__

namespace {

class Reaper {
public:
  static Reaper &instance() {
    // Leaked on purpose: destroying the joinable worker at exit would terminate().
    static auto *reaper = new Reaper();
    return *reaper;
  }

  // done == nullptr means the thread is about to finish; join it right away.
  void add(std::thread thread, std::shared_ptr<bool> done) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      entries_.push_back({std::move(thread), std::move(done)});
    }
    cv_.notify_one();
  }

  void markDone(bool &done) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      done = true;
    }
    cv_.notify_one();
  }

private:
  struct Entry {
    std::thread thread;
    std::shared_ptr<bool> done;

    bool finished() const { return !done || *done; }
  };

  Reaper() : worker_([this] { loop(); }) {}

  void loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      cv_.wait(lock, [this] {
        for (auto &e : entries_)
          if (e.finished())
            return true;
        return false;
      });
      std::list<Entry> finished;
      for (auto it = entries_.begin(); it != entries_.end();) {
        auto next = std::next(it);
        if (it->finished())
          finished.splice(finished.end(), entries_, it);
        it = next;
      }
      lock.unlock();
      for (auto &e : finished)
        e.thread.join();
      lock.lock();
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::list<Entry> entries_;
  std::thread worker_;
};

} // namespace

void runDetached(std::function<void()> fn) {
  auto done = std::make_shared<bool>(false);
  std::thread thread([fn = std::move(fn), done] {
    fn();
    Reaper::instance().markDone(*done);
  });
  Reaper::instance().add(std::move(thread), std::move(done));
}

void detach(std::thread thread) { Reaper::instance().add(std::move(thread), nullptr); }

#else

void runDetached(std::function<void()> fn) { std::thread(std::move(fn)).detach(); }

void detach(std::thread thread) { thread.detach(); }

#endif

} // namespace vkpcnx::utils
