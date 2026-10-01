#pragma once
#ifdef __SWITCH__

#include <cstdint>

// What __libnx_initheap (switch_heap.cpp) found in the inherited heap. Filled
// before stdio exists, so it's logged later from main().
struct HeapFixup {
  static constexpr int kMaxBad = 8;
  struct Block {
    uint64_t start, end;
    uint32_t perm, attr;
  };
  uint64_t heapStart = 0, heapEnd = 0;
  int count = 0; // may exceed kMaxBad
  Block bad[kMaxBad];
};

const HeapFixup &heapFixup();

extern "C" char *fake_heap_start; // newlib's malloc arena, set by __libnx_initheap
extern "C" char *fake_heap_end;

#endif
