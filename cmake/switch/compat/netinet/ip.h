/* Minimal <netinet/ip.h> for libnx: only the TOS constants libwebsockets
 * uses when applying socket options. */
#pragma once
#include <netinet/in.h>
#define IPTOS_LOWDELAY 0x10
#define IPTOS_THROUGHPUT 0x08
#define IPTOS_RELIABILITY 0x04
#define IPTOS_MINCOST 0x02
