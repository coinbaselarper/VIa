#pragma once

/*
 * Windows x64 TEB/PEB offsets the host must honour. Measured usage by the title (NOTES.md S0.6):
 * gs:[0x58] 53,246 sites, gs:[0x30] once, gs:[0x10] once (in __chkstk, patched to `ret` at load).
 * Under SHARE (S0.6) gs:[0x00] cannot be provided: macOS keeps pthread_self there.
 * Offsets are the documented Windows 10 x64 layout; the *_SIZE and NUMBER_OF_PROCESSORS values are not
 * yet checked against the title and must be confirmed by isa/ before anything depends on them.
 */

#define GTA6RT_TEB_EXCEPTION_LIST 0x000
#define GTA6RT_TEB_STACK_BASE 0x008
#define GTA6RT_TEB_STACK_LIMIT 0x010
#define GTA6RT_TEB_SELF 0x030
#define GTA6RT_TEB_ENVIRONMENT 0x038
#define GTA6RT_TEB_CLIENT_ID_PROCESS 0x040
#define GTA6RT_TEB_CLIENT_ID_THREAD 0x048
#define GTA6RT_TEB_TLS_POINTER 0x058
#define GTA6RT_TEB_PEB 0x060
#define GTA6RT_TEB_LAST_ERROR 0x068
#define GTA6RT_TEB_TLS_SLOTS 0x1480
#define GTA6RT_TEB_TLS_EXPANSION 0x1780
#define GTA6RT_TEB_SIZE 0x1838
#define GTA6RT_TEB_TLS_SLOT_COUNT 64

#define GTA6RT_PEB_IMAGE_BASE 0x010
#define GTA6RT_PEB_LDR 0x018
#define GTA6RT_PEB_PROCESS_PARAMETERS 0x020
#define GTA6RT_PEB_PROCESS_HEAP 0x030
#define GTA6RT_PEB_NUMBER_OF_PROCESSORS 0x0B8
#define GTA6RT_PEB_SIZE 0x7C8

/* S0.1: macOS TSD slots that libsystem leaves zero and that line up with the TEB offsets above. */
#define GTA6RT_SHARE_TSD_SLOT_TEB 6
#define GTA6RT_SHARE_TSD_SLOT_TLS 11
#define GTA6RT_SHARE_TSD_SLOT_PEB 12

#define GTA6RT_GUEST_PAGE_SIZE 0x1000
#define GTA6RT_GUEST_ALLOCATION_GRANULARITY 0x10000
