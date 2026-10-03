// POSIX functions referenced by libwebsockets / libjuice that libnx's newlib
// doesn't provide. Only pipe() matters at runtime (it wakes the poll loops of
// both libraries); the rest are process/user-management calls that are never
// reached on the Switch and simply report failure.
#ifdef __SWITCH__

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pwd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <borealis/core/logger.hpp>

extern "C" {

// pipe(2) emulated with a pair of connected UDP sockets on the loopback
// interface: fds[0] is the read end, fds[1] the write end. Byte counts are not
// preserved exactly (datagrams), which is fine for the wake-up-only use.
//
// Every UDP socket starts with the app-wide receive buffer, raised to 512 KB
// for video (switch_socket_config.cpp). Each lws context and libjuice agent
// owns one of these pipes, and at full size a dozen of them used up the
// socket buffer pool: socket() failed with ENOBUFS, and the context lost its
// wake-ups (cross-thread sends/closes then waited up to 30 s). A wake-up only
// ever carries 1-byte datagrams, so each socket is shrunk right away, before
// the next one is created.
namespace {
constexpr int kWakeSocketBufBytes = 4096;

int wakeSocket() {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0)
    return -1;
  int size = kWakeSocketBufBytes;
  if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof size) < 0 ||
      setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof size) < 0)
    brls::Logger::warning("pipe: cannot shrink socket buffers: {}", std::strerror(errno));
  return fd;
}
} // namespace

int pipe(int fds[2]) {
  const char *step = "socket";
  int a = wakeSocket();
  int b = a < 0 ? -1 : wakeSocket();
  if (a < 0 || b < 0)
    goto fail;
  {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    socklen_t len = sizeof(addr);
    step = "bind";
    if (bind(a, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 ||
        bind(b, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
      goto fail;
    sockaddr_in addrA{}, addrB{};
    step = "getsockname";
    len = sizeof(addrA);
    if (getsockname(a, reinterpret_cast<sockaddr *>(&addrA), &len) < 0)
      goto fail;
    len = sizeof(addrB);
    if (getsockname(b, reinterpret_cast<sockaddr *>(&addrB), &len) < 0)
      goto fail;
    step = "connect";
    if (connect(a, reinterpret_cast<sockaddr *>(&addrB), sizeof(addrB)) < 0 ||
        connect(b, reinterpret_cast<sockaddr *>(&addrA), sizeof(addrA)) < 0)
      goto fail;
  }
  fds[0] = a; // read end
  fds[1] = b; // write end
  return 0;
fail:
  {
    int err = errno;
    // lws and libjuice carry on without their wake-up pipe: cross-thread
    // sends/closes then wait for the poll loop's next timer
    brls::Logger::error("pipe: loopback UDP pair failed at {}: {}", step, std::strerror(err));
  }
  if (a >= 0)
    close(a);
  if (b >= 0)
    close(b);
  errno = ENOSYS;
  return -1;
}

long sysconf(int name) {
  (void)name;
  errno = EINVAL;
  return -1;
}

int chown(const char *path, uid_t owner, gid_t group) {
  (void)path;
  (void)owner;
  (void)group;
  errno = ENOSYS;
  return -1;
}

int setuid(uid_t uid) {
  (void)uid;
  errno = EPERM;
  return -1;
}

int setgid(gid_t gid) {
  (void)gid;
  errno = EPERM;
  return -1;
}

int initgroups(const char *user, gid_t group) {
  (void)user;
  (void)group;
  errno = EPERM;
  return -1;
}

struct passwd *getpwnam(const char *name) {
  (void)name;
  errno = ENOENT;
  return nullptr;
}

struct passwd *getpwuid(uid_t uid) {
  (void)uid;
  errno = ENOENT;
  return nullptr;
}

struct group *getgrnam(const char *name) {
  (void)name;
  errno = ENOENT;
  return nullptr;
}

struct group *getgrgid(gid_t gid) {
  (void)gid;
  errno = ENOENT;
  return nullptr;
}

// No interface enumeration on libnx; lws only needs this to bind to an
// interface by name, which the app never does.
struct ifaddrs;
int getifaddrs(struct ifaddrs **ifap) {
  *ifap = nullptr;
  errno = ENOSYS;
  return -1;
}

void freeifaddrs(struct ifaddrs *ifa) { (void)ifa; }

// libwebsockets' mbedTLS wrapper initialises mbedtls_net_context handles even
// though it does all socket I/O itself; MBEDTLS_NET_C is off in our build.
struct mbedtls_net_context_compat {
  int fd;
};
void mbedtls_net_init(struct mbedtls_net_context_compat *ctx) { ctx->fd = -1; }
void mbedtls_net_free(struct mbedtls_net_context_compat *ctx) { ctx->fd = -1; }

} // extern "C"

#endif
