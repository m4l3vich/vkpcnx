/* mbedTLS 3.6 configuration overrides for the Nintendo Switch build.
 *
 * Applied on top of the default mbedtls_config.h through
 * MBEDTLS_USER_CONFIG_FILE. The library is used by libwebsockets (TLS client
 * for the signalling WebSockets) and libdatachannel (DTLS-SRTP), curl uses
 * the libnx SSL service instead. */

/* No /dev/urandom or getrandom(): entropy comes from mbedtls_hardware_poll()
 * (vkpcnx/src/core/platform/switch_mbedtls_platform.cpp → libnx randomGet). */
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_NO_PLATFORM_ENTROPY

/* WebRTC media keying (DTLS-SRTP, RFC 5764). Off in the default config. */
#define MBEDTLS_SSL_DTLS_SRTP

/* Several threads (lws, libdatachannel, curl) use the library at once. */
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD

/* Modules that assume a Unix/Windows host. libdatachannel brings its own
 * DTLS timers and sockets, libwebsockets its own networking. */
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_SELF_TEST
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C

/* Runtime CPU feature detection needs getauxval()/sysctl(); the Tegra X1
 * (Cortex-A57) always has the ARMv8 crypto extensions, so use them
 * unconditionally for AES and SHA-256 (fast SRTP AES-GCM). */
#define MBEDTLS_AESCE_C
#define MBEDTLS_AES_USE_HARDWARE_ONLY
#define MBEDTLS_SHA256_USE_ARMV8_A_CRYPTO_ONLY
#undef MBEDTLS_SHA256_USE_ARMV8_A_CRYPTO_IF_PRESENT

/* newlib doesn't announce POSIX.1b, so provide the millisecond clock
 * ourselves (same file as mbedtls_hardware_poll). */
#define MBEDTLS_PLATFORM_MS_TIME_ALT
