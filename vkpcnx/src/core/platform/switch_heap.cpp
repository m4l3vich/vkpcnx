// Replaces libnx's __libnx_initheap to survive a dirty inherited heap.
//
// An NRO runs inside hbloader's process and gets the same heap hbmenu had.
// When launched through hbmenu's NetLoader, part of that heap can still be
// mapped as a thread stack (perm none, borrowed) — hbmenu's NetLoader thread
// isn't always gone yet. newlib's malloc doesn't know, hands those pages out,
// and the first write to them faults. So we scan the heap and give malloc only
// the largest run of plain read-write pages.
#ifdef __SWITCH__

#include <switch.h>

#include "core/platform/switch_heap.hpp"

extern "C" {

static HeapFixup g_fixup;

// Same as libnx's default, then trims the range to the largest clean run.
void __libnx_initheap(void) {
  void *addr = nullptr;
  u64 size = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (total > used + 0x200000)
      size = (total - used - 0x200000) & ~0x1FFFFFull;
    if (size == 0)
      size = 0x20000000;
    if (R_FAILED(svcSetHeapSize(&addr, size)))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  u64 start = reinterpret_cast<u64>(addr), end = start + size;
  u64 bestStart = start, bestEnd = start, runStart = start;
  for (u64 cur = start; cur < end;) {
    MemoryInfo info{};
    u32 pageInfo;
    if (R_FAILED(svcQueryMemory(&info, &pageInfo, cur)))
      break;
    u64 blockEnd = info.addr + info.size < end ? info.addr + info.size : end;
    bool clean = info.perm == Perm_Rw && info.attr == 0;
    if (!clean) {
      if (g_fixup.count < HeapFixup::kMaxBad)
        g_fixup.bad[g_fixup.count] = {cur, blockEnd, info.perm, info.attr};
      g_fixup.count++;
      runStart = blockEnd;
    } else if (blockEnd - runStart > bestEnd - bestStart) {
      bestStart = runStart;
      bestEnd = blockEnd;
    }
    cur = blockEnd;
  }

  g_fixup.heapStart = start;
  g_fixup.heapEnd = end;
  if (g_fixup.count == 0 || bestEnd == bestStart) {
    bestStart = start;
    bestEnd = end;
  }
  fake_heap_start = reinterpret_cast<char *>(bestStart);
  fake_heap_end = reinterpret_cast<char *>(bestEnd);
}

} // extern "C"

const HeapFixup &heapFixup() { return g_fixup; }

#endif
