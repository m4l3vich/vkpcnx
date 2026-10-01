/* Force-included into every libwebsockets source on the Switch build
 * (see CMakeLists.txt): the handful of Unix-isms lws 4.3 assumes that libnx /
 * newlib don't provide. */
#pragma once

/* lws 4.3 calls mbedtls_x509_get_name(), which mbedTLS 3.x still exports from
 * libmbedx509 but only declares in its private library/x509_internal.h. */
#include <mbedtls/x509.h>
int mbedtls_x509_get_name(unsigned char **p, const unsigned char *end, mbedtls_x509_name *cur);

/* Process resource limits don't exist on the Switch; lws only touches them
 * when info->rlimit_nofile is set, which this app never does. */
#include <sys/resource.h>
#ifndef RLIMIT_NOFILE
#define RLIMIT_NOFILE 7
struct rlimit {
  unsigned long rlim_cur;
  unsigned long rlim_max;
};
static inline int setrlimit(int resource, const struct rlimit *rl) {
  (void)resource;
  (void)rl;
  return 0;
}
static inline int getrlimit(int resource, struct rlimit *rl) {
  (void)resource;
  rl->rlim_cur = rl->rlim_max = 1024;
  return 0;
}
#endif
