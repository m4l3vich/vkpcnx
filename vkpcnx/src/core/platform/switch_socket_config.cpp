// Socket buffer sizes for streaming. borealis' userAppInit() initialises the
// socket service with libnx's defaults, which give each UDP socket a 42 KB
// receive buffer (0xA500). A single video keyframe arrives as a burst of
// several hundred KB, so packets were dropped whenever the WebRTC thread
// didn't drain the socket fast enough, and the picture broke up until the
// next keyframe. Every socketInitialize() call is redirected here with
// -Wl,--wrap=socketInitialize (see CMakeLists.txt) to raise the limits.
#ifdef __SWITCH__

#include <algorithm>
#include <switch.h>

extern "C" {

Result __real_socketInitialize(const SocketInitConfig *config);

Result __wrap_socketInitialize(const SocketInitConfig *config) {
  SocketInitConfig cfg = *config;
  // Transfer memory grows with sb_efficiency * (tcp max + udp buffers):
  // 8 * (0x40000 * 2 + 0x2400 + 0x80000) ≈ 8.3 MB of the app heap
  cfg.udp_rx_buf_size = std::max<u32>(cfg.udp_rx_buf_size, 0x80000);
  cfg.sb_efficiency = std::max<u32>(cfg.sb_efficiency, 8);
  return __real_socketInitialize(&cfg);
}

} // extern "C"

#endif
