#include "core/diag/log.hpp"

#include "core/diag/build_info.hpp"
#include "core/diag/redact.hpp"
#include "core/settings.hpp"

#include <borealis/core/logger.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace vkpcnx::diag {

namespace {

#ifdef _WIN32
int osOpen(const std::string &path) {
  return _open(path.c_str(), _O_WRONLY | _O_CREAT | _O_APPEND | _O_BINARY, _S_IREAD | _S_IWRITE);
}
long osWrite(int fd, const char *data, size_t size) {
  return _write(fd, data, static_cast<unsigned>(size));
}
void osSync(int fd) { _commit(fd); }
constexpr int kStderr = 2;
#else
int osOpen(const std::string &path) { return open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644); }
long osWrite(int fd, const char *data, size_t size) { return write(fd, data, size); }
void osSync(int fd) { fsync(fd); }
constexpr int kStderr = STDERR_FILENO;
#endif

void writeAll(int fd, const char *data, size_t size) {
  while (size > 0) {
    long n = osWrite(fd, data, size);
    if (n <= 0)
      return;
    data += n;
    size -= static_cast<size_t>(n);
  }
}

constexpr auto kFlushInterval = std::chrono::milliseconds(500);
constexpr int64_t kStallReportMs = 10000;
constexpr const char *kMarkerName = ".running";

int64_t steadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::steady_clock::now().time_since_epoch()
  )
    .count();
}

struct State {
  // Lock order: writeMutex, then queueMutex. Loggers only take queueMutex.
  std::mutex writeMutex;
  std::mutex queueMutex;
  std::condition_variable cv;
  std::string pending;
  size_t queuedBytes = 0;
  bool capped = false;
  bool urgent = false;
  bool stop = false;

  int fd = -1;
  std::string dir, path, markerPath;
  std::thread writer;
  brls::LogLevel consoleLevel = brls::LogLevel::LOG_INFO;

  std::atomic<int64_t> lastBeatMs{0};
  std::atomic<bool> stallReported{false};
};

// Never freed: crash handlers and late static destructors may still log
State *state = nullptr;
PreviousRun previous;

thread_local const char *threadName = nullptr;
thread_local char autoThreadName[12] = {};
std::atomic<int> threadCounter{0};

} // namespace

const char *currentThreadName() {
  if (threadName)
    return threadName;
  if (!autoThreadName[0])
    std::snprintf(autoThreadName, sizeof autoThreadName, "T%d", ++threadCounter);
  return autoThreadName;
}

namespace {

// For markTerminated(), which can't build strings in a signal handler
char markerPathC[1024] = {};

std::FILE *nullFile() {
#if defined(_WIN32)
  return std::fopen("NUL", "w");
#elif defined(__SWITCH__)
  // No /dev/null in libnx's devoptabs
  cookie_io_functions_t io{};
  io.write = [](void *, const char *, size_t size) -> ssize_t { return static_cast<ssize_t>(size); };
  return fopencookie(nullptr, "w", io);
#else
  return std::fopen("/dev/null", "w");
#endif
}

const char *levelTag(brls::LogLevel level) {
  switch (level) {
  case brls::LogLevel::LOG_ERROR:
    return "E";
  case brls::LogLevel::LOG_WARNING:
    return "W";
  case brls::LogLevel::LOG_INFO:
    return "I";
  case brls::LogLevel::LOG_DEBUG:
    return "D";
  default:
    return "V";
  }
}

// Same look as borealis' own console output, which this replaces
void printConsole(const std::tm &tm, int ms, brls::LogLevel level, const std::string &message) {
  static const char *names[] = {"ERROR", "WARNING", "INFO", "DEBUG", "VERBOSE"};
  static const char *colors[] = {
    BRLS_ERROR_COLOR, BRLS_WARNING_COLOR, BRLS_INFO_COLOR, BRLS_DEBUG_COLOR, BRLS_VERBOSE_COLOR
  };
  int i = std::min(static_cast<int>(level), 4);
  fmt::print(stdout, "{:%H:%M:%S}.{:03d}\033{}[{}]\033[0m {}\n", tm, ms, colors[i], names[i], message);
#ifdef _WIN32
  std::fflush(stdout);
#endif
}

void enqueue(std::string text, bool urgent) {
  {
    std::lock_guard<std::mutex> lock(state->queueMutex);
    state->queuedBytes += text.size();
    state->pending += text;
    state->urgent = state->urgent || urgent;
  }
  if (urgent)
    state->cv.notify_one();
}

// Runs under borealis' logger mutex, so lines arrive one at a time and in order
void onLog(brls::Logger::TimePoint when, brls::LogLevel level, std::string message) {
  auto ms = static_cast<int>(
    std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()).count() % 1000
  );
  std::tm tm = fmt::localtime(std::chrono::system_clock::to_time_t(when));

  if (level <= state->consoleLevel)
    printConsole(tm, ms, level, message);

  // borealis' GLFW input logs every key press; in a stream that is whatever
  // the user types on the remote PC, passwords included
  if (message.rfind("Key: ", 0) == 0)
    return;

  bool important = level <= brls::LogLevel::LOG_WARNING;
  {
    std::lock_guard<std::mutex> lock(state->queueMutex);
    if (state->queuedBytes > kMaxLogBytes && !important) {
      if (!state->capped) {
        state->capped = true;
        state->pending += "--- log size limit reached, only warnings and errors from here ---\n";
      }
      return;
    }
  }

  std::string prefix =
    fmt::format("{:%H:%M:%S}.{:03d} {} {:<8} ", tm, ms, levelTag(level), currentThreadName());
  std::string body = redact(message);
  std::string line;
  line.reserve(prefix.size() + body.size() + 1);
  line += prefix;
  // Multi-line messages (SDP dumps) are indented under their header line
  size_t start = 0;
  while (true) {
    size_t nl = body.find('\n', start);
    std::string_view seg(body.data() + start, (nl == std::string::npos ? body.size() : nl) - start);
    if (!seg.empty() && seg.back() == '\r')
      seg.remove_suffix(1);
    if (start > 0)
      line.append(prefix.size(), ' ');
    line.append(seg);
    line += '\n';
    if (nl == std::string::npos || nl + 1 >= body.size())
      break;
    start = nl + 1;
  }
  enqueue(std::move(line), important);
}

void drain() {
  std::lock_guard<std::mutex> writeLock(state->writeMutex);
  std::string batch;
  {
    std::lock_guard<std::mutex> lock(state->queueMutex);
    batch.swap(state->pending);
    state->urgent = false;
  }
  if (state->fd >= 0 && !batch.empty())
    writeAll(state->fd, batch.data(), batch.size());
}

void checkStall() {
  int64_t beat = state->lastBeatMs.load();
  if (beat == 0 || state->stallReported)
    return;
  int64_t stalled = steadyMs() - beat;
  if (stalled >= kStallReportMs) {
    state->stallReported = true;
    brls::Logger::warning(
      "Watchdog: main loop blocked for {} s (expected while minimized or in the HOME menu)",
      stalled / 1000
    );
  }
}

void writerLoop() {
  setThreadName("logwrite");
  while (true) {
    bool stop;
    {
      std::unique_lock<std::mutex> lock(state->queueMutex);
      state->cv.wait_for(lock, kFlushInterval, [] { return state->urgent || state->stop; });
      stop = state->stop;
    }
    checkStall();
    drain();
    if (stop)
      return;
  }
}

std::string timestampForFile() {
  std::time_t now = std::time(nullptr);
  std::tm tm = fmt::localtime(now);
  return fmt::format("{:%Y%m%d-%H%M%S}", tm);
}

brls::LogLevel parseLevel(int argc, char *argv[]) {
  // --log-level=X wins over VKPCNX_LOG_LEVEL; the Switch has no environment,
  // but nxlink passes arguments (nxlink app.nro -- --log-level=VERBOSE)
  const char *name = std::getenv("VKPCNX_LOG_LEVEL");
  for (int i = 1; i < argc; i++)
    if (std::strncmp(argv[i], "--log-level=", 12) == 0)
      name = argv[i] + 12;
  if (!name)
    return brls::LogLevel::LOG_INFO;
  std::string_view v(name);
  if (v == "ERROR")
    return brls::LogLevel::LOG_ERROR;
  if (v == "WARNING")
    return brls::LogLevel::LOG_WARNING;
  if (v == "DEBUG")
    return brls::LogLevel::LOG_DEBUG;
  if (v == "VERBOSE")
    return brls::LogLevel::LOG_VERBOSE;
  return brls::LogLevel::LOG_INFO;
}

void pruneLogs() {
  auto files = logFiles();
  for (size_t i = kMaxLogFiles; i < files.size(); i++) {
    std::error_code ec;
    fs::remove(files[i], ec);
  }
}

// Reads the previous run's marker; its log ending in a crash section tells a
// crash apart from a hang or a kill
void readPreviousRun() {
  std::ifstream marker(state->markerPath);
  if (!marker)
    return;
  previous.unclean = true;
  std::string name;
  std::getline(marker, name);
  if (name.empty())
    return;
  previous.logPath = state->dir + "/" + name;
  std::ifstream log(previous.logPath, std::ios::binary);
  if (!log)
    return;
  log.seekg(0, std::ios::end);
  auto size = static_cast<long long>(log.tellg());
  log.seekg(std::max(0LL, size - 64 * 1024));
  std::stringstream tail;
  tail << log.rdbuf();
  previous.crashed = tail.str().find("*** CRASH") != std::string::npos;
}

std::string homeDirectory() {
#if defined(_WIN32)
  const char *home = std::getenv("USERPROFILE");
#elif defined(__SWITCH__)
  const char *home = nullptr;
#else
  const char *home = std::getenv("HOME");
#endif
  return home ? home : "";
}

void writeHeader(int argc, char *argv[]) {
  std::time_t now = std::time(nullptr);
  std::tm tm = fmt::localtime(now);
  char tz[8] = {};
  std::strftime(tz, sizeof tz, "%z", &tm);
  std::string h = fmt::format("=== vkpcnx log, started {:%Y-%m-%d %H:%M:%S} {} ===\n", tm, tz);
  for (const auto &[key, value] : systemInfo())
    h += fmt::format("{:<16} {}\n", key + ":", value);
  std::string args;
  for (int i = 1; i < argc; i++)
    args += std::string(i > 1 ? " " : "") + argv[i];
  h += fmt::format("{:<16} {}\n", "arguments:", args.empty() ? "-" : redact(args));
  h += fmt::format("{:<16} {}\n", "console level:", levelTag(state->consoleLevel));
  if (previous.unclean)
    h += fmt::format(
      "{:<16} did not exit cleanly{} ({})\n",
      "previous run:",
      previous.crashed ? ", crashed" : "",
      previous.logPath.empty() ? "no log" : fs::path(previous.logPath).filename().string()
    );
  h += "===\n";
  enqueue(std::move(h), true);
}

} // namespace

void initLogging(int argc, char *argv[]) {
  if (state)
    return;
  state = new State();
  threadName = "main";
  state->consoleLevel = parseLevel(argc, argv);
  setRedactedHome(homeDirectory());

  // Everything down to DEBUG is generated for the file; the console filters
  brls::Logger::setLogLevel(std::max(state->consoleLevel, brls::LogLevel::LOG_DEBUG));
  if (std::FILE *devnull = nullFile())
    brls::Logger::setLogOutput(devnull);
  brls::Logger::getLogEvent()->subscribe(onLog);

  state->dir = Settings::configDir() + "/logs";
  std::error_code ec;
  fs::create_directories(state->dir, ec);
  state->markerPath = state->dir + "/" + kMarkerName;
  std::snprintf(markerPathC, sizeof markerPathC, "%s", state->markerPath.c_str());
  readPreviousRun();

  std::string base = state->dir + "/vkpcnx-" + timestampForFile();
  state->path = base + ".log";
  for (int i = 2; fs::exists(state->path, ec); i++)
    state->path = base + "-" + std::to_string(i) + ".log";
  state->fd = osOpen(state->path);
  pruneLogs();

  if (std::ofstream marker(state->markerPath, std::ios::trunc); marker)
    marker << fs::path(state->path).filename().string() << '\n';

  writeHeader(argc, argv);
  state->writer = std::thread(writerLoop);

  if (state->fd < 0)
    brls::Logger::error("Log: cannot open {}", state->path);
}

void shutdownLogging() {
  if (!state || state->stop)
    return;
  state->lastBeatMs = 0;
  brls::Logger::info("Log: clean exit");
  {
    std::lock_guard<std::mutex> lock(state->queueMutex);
    state->stop = true;
  }
  state->cv.notify_one();
  if (state->writer.joinable())
    state->writer.join();
  drain(); // anything logged while the writer was stopping
  std::error_code ec;
  fs::remove(state->markerPath, ec);
}

void setThreadName(const char *name) { threadName = name; }

void flushLogs() {
  if (state)
    drain();
}

void heartbeat() {
  if (!state)
    return;
  int64_t now = steadyMs();
  int64_t last = state->lastBeatMs.exchange(now);
  if (state->stallReported.exchange(false) && last != 0)
    brls::Logger::warning("Watchdog: main loop resumed after {} s", (now - last) / 1000);
}

std::string logDir() { return state ? state->dir : Settings::configDir() + "/logs"; }

std::string currentLogPath() { return state ? state->path : ""; }

std::vector<std::string> logFiles() {
  std::vector<std::string> files;
  std::error_code ec;
  for (fs::directory_iterator it(logDir(), ec), end; !ec && it != end; it.increment(ec)) {
    std::string name = it->path().filename().string();
    if (name.rfind("vkpcnx-", 0) == 0 && it->path().extension() == ".log")
      files.push_back(it->path().string());
  }
  // vkpcnx-YYYYMMDD-HHMMSS[-n].log sorts chronologically
  std::sort(files.rbegin(), files.rend());
  return files;
}

const PreviousRun &previousRun() { return previous; }

void dismissPreviousRun() { previous = PreviousRun{}; }

void crashWrite(const char *text) {
  size_t len = std::strlen(text);
  writeAll(kStderr, text, len);
  if (!state || state->fd < 0)
    return;
  // The crashing thread may hold either lock; never wait for them
  if (state->queueMutex.try_lock()) {
    if (!state->pending.empty())
      writeAll(state->fd, state->pending.data(), state->pending.size());
    state->pending.clear();
    state->queueMutex.unlock();
  }
  writeAll(state->fd, text, len);
}

void crashWritef(const char *format, ...) {
  static char buf[1024];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buf, sizeof buf, format, args);
  va_end(args);
  crashWrite(buf);
}

void crashFinish() {
  if (state && state->fd >= 0)
    osSync(state->fd);
}

void markTerminated() {
  if (markerPathC[0])
    std::remove(markerPathC);
}

} // namespace vkpcnx::diag
