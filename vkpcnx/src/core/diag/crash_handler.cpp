#include "core/diag/crash_handler.hpp"

#include "core/diag/build_info.hpp"
#include "core/diag/log.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#if defined(__SWITCH__)
#include <switch.h>
#include <unwind.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <execinfo.h>
#include <unistd.h>
#endif

namespace vkpcnx::diag {

namespace {

std::atomic<bool> inCrash{false};
char buildLine[512] = {};
constexpr int kMaxFrames = 64;

#if !defined(__SWITCH__) && !defined(_WIN32)
const char *baseName(const char *path) {
  const char *slash = path ? std::strrchr(path, '/') : nullptr;
  return slash ? slash + 1 : (path ? path : "?");
}
#endif

#if defined(__SWITCH__)

struct UnwindState {
  uintptr_t base;
  int index;
};

_Unwind_Reason_Code logFrame(_Unwind_Context *ctx, void *arg) {
  auto *s = static_cast<UnwindState *>(arg);
  uintptr_t pc = _Unwind_GetIP(ctx);
  // The NRO is linked at 0, so pc - load base is the address to feed addr2line
  crashWritef("  bt #%d %#lx vkpcnx.nro+%#lx\n", s->index++, pc, pc - s->base);
  return s->index < kMaxFrames ? _URC_NO_REASON : _URC_END_OF_STACK;
}

#elif defined(_WIN32)

void writeFrame(int index, uintptr_t pc) {
  HMODULE module = nullptr;
  char path[MAX_PATH] = "?";
  if (GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(pc),
        &module
      ))
    GetModuleFileNameA(module, path, sizeof path);
  const char *name = std::strrchr(path, '\\');
  name = name ? name + 1 : path;
  crashWritef(
    "  bt #%d %#llx %s+%#llx\n",
    index,
    static_cast<unsigned long long>(pc),
    name,
    static_cast<unsigned long long>(pc - reinterpret_cast<uintptr_t>(module))
  );
}

#if defined(_M_X64) || defined(__x86_64__)
// Walks the faulting thread's stack from the exception context, so the trace
// starts at the faulting instruction rather than inside the filter
void writeContextBacktrace(CONTEXT ctx) {
  for (int i = 0; i < kMaxFrames && ctx.Rip; i++) {
    writeFrame(i, ctx.Rip);
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
    if (!fn) { // leaf function: the return address is at the top of the stack
      ctx.Rip = *reinterpret_cast<DWORD64 *>(ctx.Rsp);
      ctx.Rsp += 8;
      continue;
    }
    PVOID handlerData = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(
      UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr
    );
  }
}
#endif

LONG WINAPI unhandledException(EXCEPTION_POINTERS *ep) {
  if (!inCrash.exchange(true)) {
    const EXCEPTION_RECORD *r = ep->ExceptionRecord;
    crashWritef(
      "\n*** CRASH: exception %#lx at %p, thread %s\n",
      static_cast<unsigned long>(r->ExceptionCode),
      r->ExceptionAddress,
      currentThreadName()
    );
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
      crashWritef(
        "  %s of address %#llx\n",
        r->ExceptionInformation[0] == 0   ? "read"
        : r->ExceptionInformation[0] == 8 ? "execute"
                                          : "write",
        static_cast<unsigned long long>(r->ExceptionInformation[1])
      );
    writeCrashBuildLine();
#if defined(_M_X64) || defined(__x86_64__)
    writeContextBacktrace(*ep->ContextRecord);
#else
    writeBacktrace();
#endif
    crashFinish();
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

void abortHandler(int) {
  if (!inCrash.exchange(true)) {
    crashWritef("\n*** CRASH: abort(), thread %s\n", currentThreadName());
    writeCrashBuildLine();
    writeBacktrace();
    crashFinish();
  }
  std::signal(SIGABRT, SIG_DFL);
  std::raise(SIGABRT);
}

#else // POSIX desktop

const char *signalName(int sig) {
  switch (sig) {
  case SIGSEGV:
    return "SIGSEGV";
  case SIGBUS:
    return "SIGBUS";
  case SIGILL:
    return "SIGILL";
  case SIGFPE:
    return "SIGFPE";
  case SIGABRT:
    return "SIGABRT";
  case SIGTERM:
    return "SIGTERM";
  case SIGINT:
    return "SIGINT";
  case SIGHUP:
    return "SIGHUP";
  default:
    return "signal";
  }
}

void fatalSignal(int sig, siginfo_t *info, void *) {
  if (!inCrash.exchange(true)) {
    crashWritef(
      "\n*** CRASH: %s (%d) at address %p, thread %s\n",
      signalName(sig),
      sig,
      info ? info->si_addr : nullptr,
      currentThreadName()
    );
    writeCrashBuildLine();
    writeBacktrace();
    crashFinish();
  }
  // SA_RESETHAND restored the default action: re-raise for the OS crash
  // reporter / core dump
  raise(sig);
}

void terminationSignal(int sig) {
  crashWritef("\n*** terminated by %s\n", signalName(sig));
  markTerminated();
  signal(sig, SIG_DFL);
  raise(sig);
}

alignas(16) char altStack[64 * 1024];

#endif

void onTerminate() {
  if (!inCrash.exchange(true)) {
    const char *what = "called without an exception";
    std::string message;
    if (auto ep = std::current_exception()) {
      try {
        std::rethrow_exception(ep);
      } catch (const std::exception &e) {
        message = std::string("uncaught exception: ") + e.what();
      } catch (...) {
        message = "uncaught non-std exception";
      }
      what = message.c_str();
    }
    crashWritef("\n*** CRASH: std::terminate: %s, thread %s\n", what, currentThreadName());
    writeCrashBuildLine();
    writeBacktrace();
    crashFinish();
  }
  // libnx abort() runs userAppExit and leaves no crash report; elsewhere the
  // SIGABRT handler sees inCrash and only re-raises
  std::abort();
}

} // namespace

void writeCrashBuildLine() { crashWrite(buildLine); }

void writeBacktrace() {
#if defined(__SWITCH__)
  UnwindState s{buildInfo().moduleBase, 0};
  _Unwind_Backtrace(logFrame, &s);
#elif defined(_WIN32)
  void *frames[kMaxFrames];
  USHORT n = RtlCaptureStackBackTrace(0, kMaxFrames, frames, nullptr);
  for (USHORT i = 0; i < n; i++)
    writeFrame(i, reinterpret_cast<uintptr_t>(frames[i]));
#else
  void *frames[kMaxFrames];
  int n = backtrace(frames, kMaxFrames);
  for (int i = 0; i < n; i++) {
    Dl_info dl{};
    auto pc = reinterpret_cast<uintptr_t>(frames[i]);
    if (dladdr(frames[i], &dl) && dl.dli_fbase)
      crashWritef(
        "  bt #%d %#lx %s+%#lx %s\n",
        i,
        static_cast<unsigned long>(pc),
        baseName(dl.dli_fname),
        static_cast<unsigned long>(pc - reinterpret_cast<uintptr_t>(dl.dli_fbase)),
        dl.dli_sname ? dl.dli_sname : ""
      );
    else
      crashWritef("  bt #%d %#lx ?\n", i, static_cast<unsigned long>(pc));
  }
#endif
}

void installCrashHandlers() {
  const auto &b = buildInfo();
  std::snprintf(
    buildLine,
    sizeof buildLine,
    "  build: %s %s %s build-id %s module-base %#llx link-base %#llx\n",
    b.version.c_str(),
    b.git.c_str(),
    b.platform.c_str(),
    b.buildId.empty() ? "unknown" : b.buildId.c_str(),
    static_cast<unsigned long long>(b.moduleBase),
    static_cast<unsigned long long>(b.linkBase)
  );

  std::set_terminate(onTerminate);

#if defined(_WIN32)
  SetUnhandledExceptionFilter(unhandledException);
  std::signal(SIGABRT, abortHandler);
#elif !defined(__SWITCH__)
  // Load libgcc's unwinder now: backtrace() would otherwise dlopen it (and
  // malloc) the first time it runs, inside the signal handler
  void *warmup[1];
  backtrace(warmup, 1);

  stack_t ss{};
  ss.ss_sp = altStack;
  ss.ss_size = sizeof altStack;
  sigaltstack(&ss, nullptr); // main thread only: catches its stack overflows

  struct sigaction sa {};
  sa.sa_sigaction = fatalSignal;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
  sigemptyset(&sa.sa_mask);
  for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT})
    sigaction(sig, &sa, nullptr);
  for (int sig : {SIGTERM, SIGINT, SIGHUP})
    signal(sig, terminationSignal);
#endif
}

} // namespace vkpcnx::diag
