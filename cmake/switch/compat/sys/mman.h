/* libwebsockets' unix platform header includes <sys/mman.h> unconditionally
 * but never calls mmap(); libnx has no such header, so this stub satisfies
 * the include. Only on the include path of the websockets target. */
#pragma once
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_FAILED ((void *)-1)
