#ifndef GTA6RT_GOVERNOR_H
#define GTA6RT_GOVERNOR_H

/*
 * Host memory governor (port/host/src/governor): reclaimers other modules register.
 *
 * The governor thread calls a reclaimer when the process nears its memory budget or the OS reports pressure. It is
 * never called on a title thread. `goal_bytes` is how much the governor would like back (0 = whatever is cheap);
 * the return value is the bytes the reclaimer believes it released (the governor also measures the footprint before
 * and after and logs both). A reclaimer must not block on title threads; it may take its own locks briefly.
 *
 * The symbols live in the gta6rt executable. They are weak imports so a module's unit tests link without the host:
 * test `if (gta6rt_governor_register)` before calling.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum gta6rt_governor_action {
    GTA6RT_GOV_LAYER_EVICT = 0,   /* (a) the D3D12 layer drops copies of title memory it can recreate */
    GTA6RT_GOV_COMPILE_SHRINK = 1, /* (b) fewer pipeline compiler threads, no speculative warm-up */
    GTA6RT_GOV_HOST_CACHES = 2,   /* (c) host caches: SPIR-V, DirectStorage staging window, trace rings */
    GTA6RT_GOV_POOL_PAGES = 3,    /* (d) title pool pages the title released */
    GTA6RT_GOV_ACTION_COUNT = 4
} gta6rt_governor_action;

/* level: 1 soft (near the budget), 2 hard (at it or OS warning), 3 critical (over it or OS critical). Level 0 is
 * the all-clear: a reclaimer that throttled something (b) restores it then. */
typedef uint64_t (*gta6rt_governor_reclaim_fn)(void* ctx, uint32_t level, uint64_t goal_bytes);

/* Returns 0 on success, -1 when the action id is invalid or 8 reclaimers are already registered for it. */
__attribute__((weak_import)) int gta6rt_governor_register(gta6rt_governor_action action, const char* name,
                                                          gta6rt_governor_reclaim_fn fn, void* ctx);

/* The level now (0..3); modules may consult it before speculative work. */
__attribute__((weak_import)) uint32_t gta6rt_governor_level(void);

#ifdef __cplusplus
}
#endif

#endif
