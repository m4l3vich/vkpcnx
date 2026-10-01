#pragma once

#include <functional>
#include <thread>

namespace vkpcnx::utils {

// std::thread::detach() throws ENOSYS on Switch because libnx doesn't implement
// pthread_detach. On Switch these hand the thread to a reaper that joins it once
// it's done; elsewhere they just detach.

// Runs fn on a new thread that nobody has to join.
void runDetached(std::function<void()> fn);

// Gives up a thread that is about to finish, e.g. from a stop() running on that
// thread itself. The reaper blocks joining it, so don't pass long-running threads.
void detach(std::thread thread);

} // namespace vkpcnx::utils
