/* <endian.h> for devkitA64/newlib, which only ships <machine/endian.h>.
 * Provides the glibc-style __BYTE_ORDER macros protobuf and others test. */
#pragma once
#include <machine/endian.h>
#ifndef __LITTLE_ENDIAN
#define __LITTLE_ENDIAN _LITTLE_ENDIAN
#endif
#ifndef __BIG_ENDIAN
#define __BIG_ENDIAN _BIG_ENDIAN
#endif
#ifndef __BYTE_ORDER
#define __BYTE_ORDER _BYTE_ORDER
#endif
