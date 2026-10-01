#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Persistent, redacted logging on top of brls::Logger.
//
// borealis' logger runs at DEBUG (or VERBOSE when asked for) so the log file
// always has the detail a bug report needs, while the console keeps the level
// requested with --log-level= / VKPCNX_LOG_LEVEL (default INFO). Each run gets
// its own file in logDir(); the newest kMaxLogFiles are kept.
//
// A marker file exists while the app runs. If it is still there at the next
// launch, that run crashed, hung or was killed, and previousRun() says so.
namespace vkpcnx::diag {

constexpr size_t kMaxLogFiles = 8;
constexpr size_t kMaxLogBytes = 8 * 1024 * 1024; // past this only warnings/errors

// Parses the log level, opens the log file and hooks brls::Logger. Call first
// thing in main().
void initLogging(int argc, char *argv[]);
// Clean exit: flushes, closes, removes the running marker.
void shutdownLogging();

// Name shown in the thread column of the log file (≤ 8 chars read best).
// Threads that never call it are shown as T1, T2, …
void setThreadName(const char *name);
const char *currentThreadName();

// Writes everything queued so far to the file before returning.
void flushLogs();

// Main-loop tick: lets the writer thread log UI stalls (deadlocks, long
// blocking calls on the UI thread) that would otherwise look like a crash.
void heartbeat();

std::string logDir();
std::string currentLogPath();
// Log files, newest first (the current one included)
std::vector<std::string> logFiles();

struct PreviousRun {
  bool unclean = false;  // the marker survived: crash, hang, kill, power loss
  bool crashed = false;  // ...and its log ends with a crash section
  std::string logPath;   // that run's log
};
const PreviousRun &previousRun();
// The report offer for an unclean exit is shown once
void dismissPreviousRun();

// Crash path: best effort, no allocation; usable from a signal handler or the
// libnx exception handler. Writes queued lines and then `text` straight to
// the log file (and to stderr).
void crashWrite(const char *text);
void crashWritef(const char *fmt, ...)
#if defined(__GNUC__)
  __attribute__((format(printf, 1, 2)))
#endif
  ;
// fsync the log after a crash section
void crashFinish();
// Killed on purpose (SIGTERM, Ctrl+C): not worth a report offer next launch.
// Async-signal-safe.
void markTerminated();

} // namespace vkpcnx::diag
