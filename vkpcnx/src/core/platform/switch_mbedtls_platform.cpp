// mbedTLS platform hooks for the Switch build (see
// cmake/switch/mbedtls_user_config.h): entropy from the kernel CSPRNG and a
// monotonic millisecond clock for DTLS timers.
#ifdef __SWITCH__

#include <cstdint>
#include <switch.h>
#include <time.h>

extern "C" int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen) {
  (void)data;
  randomGet(output, len);
  if (olen)
    *olen = len;
  return 0;
}

extern "C" int64_t mbedtls_ms_time(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
  return static_cast<int64_t>(time(nullptr)) * 1000;
}

#endif
