// Logs hard faults (data aborts etc.) to the log file and stderr (the nxlink
// console) before Atmosphère writes its crash report. The report has
// registers and a stack trace but not the state of the faulting memory, which
// is what tells a null deref from a use-after-free from memory lent to a
// service. Everything goes through crashWritef: no allocation, no locks held.
#ifdef __SWITCH__

#include "core/diag/crash_handler.hpp"
#include "core/diag/log.hpp"

#include <switch.h>

using vkpcnx::diag::crashWritef;

extern "C" {

extern char *fake_heap_start;
extern char *fake_heap_end;

alignas(16) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

static void logRegion(const char *what, u64 addr) {
  MemoryInfo info{};
  u32 pageInfo = 0;
  if (R_FAILED(svcQueryMemory(&info, &pageInfo, addr))) {
    crashWritef("  %s %#lx: svcQueryMemory failed\n", what, addr);
    return;
  }
  crashWritef("  %s %#lx: region %#lx..%#lx type=%#x perm=%#x attr=%#x ipc=%u dev=%u\n", what, addr,
         info.addr, info.addr + info.size, info.type, info.perm, info.attr, info.ipc_refcount,
         info.device_refcount);
}

static u64 moduleBase() {
  MemoryInfo info{};
  u32 pageInfo;
  svcQueryMemory(&info, &pageInfo, reinterpret_cast<u64>(&moduleBase));
  return info.addr;
}

void __libnx_exception_handler(ThreadExceptionDump *ctx) {
  u64 far = ctx->far.x;
  crashWritef("\n*** CRASH: exception desc=%#x esr=%#x pc=%#lx lr=%#lx far=%#lx, thread %s\n",
              ctx->error_desc, ctx->esr, ctx->pc.x, ctx->lr.x, far,
              vkpcnx::diag::currentThreadName());
  vkpcnx::diag::writeCrashBuildLine();
  crashWritef("  heap %p..%p far=heap+%#lx pc=+%#lx lr=+%#lx\n", fake_heap_start, fake_heap_end,
         far - reinterpret_cast<u64>(fake_heap_start), ctx->pc.x - moduleBase(),
         ctx->lr.x - moduleBase());
  logRegion("far", far);
  if (far >= 0x1000)
    logRegion("far-page", far - 0x1000);
  logRegion("far+page", far + 0x1000);

  u64 total = 0, used = 0;
  svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  crashWritef("  memory used %lu / %lu MiB\n", used >> 20, total >> 20);
  // Registers of the faulting frame; the unwinder can't walk past the fault
  for (int i = 0; i < 29; i++)
    crashWritef("  x%-2d %#018lx%s", i, ctx->cpu_gprs[i].x, i % 4 == 3 ? "\n" : "");
  crashWritef("\n  fp %#018lx sp %#018lx\n", ctx->fp.x, ctx->sp.x);
  vkpcnx::diag::crashFinish();

  svcSleepThread(500'000'000); // let the nxlink socket drain
  // Returning from the handler would resume at the faulting instruction; abort
  // instead so Atmosphère still writes its crash report.
  diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_ShouldNotHappen));
}

} // extern "C"

#endif
