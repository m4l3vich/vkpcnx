/* Minimal <sys/un.h> for libwebsockets on libnx (unix sockets are not
 * supported by the Switch BSD service; LWS_UNIX_SOCK is off). */
#pragma once
#include <sys/socket.h>
struct sockaddr_un {
  unsigned char sun_len;
  sa_family_t sun_family;
  char sun_path[104];
};
