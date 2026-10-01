// libnx's getnameinfo() forwards to the sfdnsres service, which ignores
// NI_NUMERICHOST and returns the reverse-DNS name of the address. libdatachannel
// and libjuice rely on the flag to turn resolved candidates back into IP
// strings, so remote ICE candidates came out as PTR names (which don't resolve
// forward) and the connectivity checks never started. Every getnameinfo()
// reference is redirected here with -Wl,--wrap=getnameinfo (see CMakeLists.txt).
#ifdef __SWITCH__

#include <arpa/inet.h>
#include <cstdio>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

extern "C" {

int __real_getnameinfo(
  const struct sockaddr *sa, socklen_t salen, char *host, socklen_t hostlen, char *serv,
  socklen_t servlen, int flags
);

int __wrap_getnameinfo(
  const struct sockaddr *sa, socklen_t salen, char *host, socklen_t hostlen, char *serv,
  socklen_t servlen, int flags
) {
  if (!(flags & NI_NUMERICHOST) || !sa)
    return __real_getnameinfo(sa, salen, host, hostlen, serv, servlen, flags);

  const void *addr;
  unsigned port;
  if (sa->sa_family == AF_INET && salen >= static_cast<socklen_t>(sizeof(sockaddr_in))) {
    auto *in = reinterpret_cast<const sockaddr_in *>(sa);
    addr = &in->sin_addr;
    port = ntohs(in->sin_port);
  }
#ifdef AF_INET6
  else if (sa->sa_family == AF_INET6 && salen >= static_cast<socklen_t>(sizeof(sockaddr_in6))) {
    auto *in6 = reinterpret_cast<const sockaddr_in6 *>(sa);
    addr = &in6->sin6_addr;
    port = ntohs(in6->sin6_port);
  }
#endif
  else {
    return EAI_FAMILY;
  }

  if (host && hostlen && !inet_ntop(sa->sa_family, addr, host, hostlen))
    return EAI_OVERFLOW;
  // Service names are meaningless on the Switch; the port is always numeric
  if (serv && servlen) {
    int n = std::snprintf(serv, servlen, "%u", port);
    if (n < 0 || static_cast<socklen_t>(n) >= servlen)
      return EAI_OVERFLOW;
  }
  return 0;
}

} // extern "C"

#endif
