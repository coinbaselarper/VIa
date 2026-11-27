#pragma once

/*
 * Shared definitions for the module interfaces in include/gta6rt/. These headers are plain C, do not
 * include windows.h, and are safe to include from .c, .cpp and (for os.h) .mm files.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define GTA6RT_API_BEGIN extern "C" {
#define GTA6RT_API_END }
#else
#define GTA6RT_API_BEGIN
#define GTA6RT_API_END
#endif

#if defined(__x86_64__)
#define GTA6RT_GUEST_ABI __attribute__((ms_abi))
#else
#define GTA6RT_GUEST_ABI
#endif

/* Guest virtual address. Guest and host share one address space on both backends. */
typedef uint64_t gta6rt_gva;

/* Status codes: 0 is success, negative is failure. */
typedef int32_t gta6rt_status;
#define GTA6RT_OK 0
#define GTA6RT_E_NOT_IMPLEMENTED (-1)
#define GTA6RT_E_INVALID_ARG (-2)
#define GTA6RT_E_NO_MEMORY (-3)
#define GTA6RT_E_NOT_FOUND (-4)
#define GTA6RT_E_ALREADY_EXISTS (-5)
#define GTA6RT_E_UNSUPPORTED (-6)
#define GTA6RT_E_TIMEOUT (-7)
#define GTA6RT_E_IO (-8)
#define GTA6RT_E_BAD_FORMAT (-9)
#define GTA6RT_E_ADDRESS_IN_USE (-10)
#define GTA6RT_E_ACCESS (-11)
#define GTA6RT_E_BUSY (-12)
#define GTA6RT_E_ABANDONED (-13)

GTA6RT_API_BEGIN
/* Implemented by os/. Stable English name for a status code. */
const char* gta6rt_status_name(gta6rt_status s);
GTA6RT_API_END
