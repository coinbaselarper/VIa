#ifndef GTA6RT_HUD_STATS_H
#define GTA6RT_HUD_STATS_H

/*
 * Lock-free stats block of the performance overlay. Plain C, header only, callable from any thread and from ms_abi
 * code alike (ordinary inline functions on plain 64-bit integers, accessed with __atomic builtins; relaxed ordering).
 * Producers never block and never allocate. Until the overlay switches the block active, every note/add call is one
 * relaxed load and a branch.
 *
 * Cumulative fields (presents, draws, ...) only grow; the overlay samples them on a timer and works with deltas, so
 * a producer may either add events as they happen or store a running total it already keeps (gta6rt_hud_publish).
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GTA6RT_HUD_STATS_VERSION 1
#define GTA6RT_HUD_STRING_LEN 64

enum {
    GTA6RT_HUD_HAVE_SUBMISSIONS = 1u << 0,
    GTA6RT_HUD_HAVE_EXEC_CPU = 1u << 1,
    GTA6RT_HUD_HAVE_DISPATCHES = 1u << 2,
    GTA6RT_HUD_HAVE_GPU = 1u << 3,
    GTA6RT_HUD_HAVE_LATENCY = 1u << 4,
    GTA6RT_HUD_HAVE_PIPELINES = 1u << 5,
    GTA6RT_HUD_HAVE_TEXTURES = 1u << 6,
    GTA6RT_HUD_HAVE_IMPORTS = 1u << 7,
    GTA6RT_HUD_HAVE_MEMORY = 1u << 8
};

typedef struct gta6rt_hud_string {
    uint32_t seq;
    char text[GTA6RT_HUD_STRING_LEN];
} gta6rt_hud_string;

typedef struct gta6rt_hud_stats {
    uint32_t version;
    uint32_t active;
    uint64_t have;

    uint64_t presents;
    uint64_t frame_intervals;
    uint64_t frame_ns_sum;
    uint64_t frame_ns_max;
    uint64_t last_present_ns;

    uint64_t draws;
    uint64_t dispatches;
    uint64_t submissions;
    uint64_t exec_cpu_ns;

    uint64_t gpu_ns;
    uint64_t gpu_samples;
    uint64_t gpu_last_done_ns;

    uint64_t latency_ns_sum;
    uint64_t latency_samples;
    uint64_t latency_ns_max;

    uint64_t pipelines_built;
    uint64_t pipelines_pending;
    uint64_t pipeline_cache_hits;
    uint64_t pso_failures;
    uint64_t textures_waiting;
    uint64_t guest_imports;

    gta6rt_hud_string state;
    gta6rt_hud_string driver;
    gta6rt_hud_string profile;

    uint64_t mem_budget;
    uint64_t gpu_bytes;
    uint64_t gpu_limit;
    uint64_t governor_level;
    uint64_t mem_warning;
    gta6rt_hud_string mem_note;
} gta6rt_hud_stats;

/* One weak definition per translation unit; the linker keeps a single copy, so C and C++ producers share the block. */
__attribute__((weak)) gta6rt_hud_stats gta6rt_hud_stats_storage;

static inline gta6rt_hud_stats* gta6rt_hud_stats_block(void) { return &gta6rt_hud_stats_storage; }

#define GTA6RT_HUD_LOAD_(p) __atomic_load_n((p), __ATOMIC_RELAXED)
#define GTA6RT_HUD_STORE_(p, v) __atomic_store_n((p), (v), __ATOMIC_RELAXED)
#define GTA6RT_HUD_ADD_(p, v) ((void)__atomic_fetch_add((p), (v), __ATOMIC_RELAXED))
#define GTA6RT_HUD_OR_(p, v) ((void)__atomic_fetch_or((p), (v), __ATOMIC_RELAXED))

static inline uint64_t gta6rt_hud_now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static inline int gta6rt_hud_active(void) { return GTA6RT_HUD_LOAD_(&gta6rt_hud_stats_block()->active) != 0; }
/* Switching on clears the timestamps and the frame maximum kept while off, so the first window does not see a stale gap. */
static inline void gta6rt_hud_set_active(int on) {
    gta6rt_hud_stats* b = gta6rt_hud_stats_block();
    const uint32_t before = __atomic_exchange_n(&b->active, on ? 1u : 0u, __ATOMIC_RELAXED);
    if (on && !before) {
        GTA6RT_HUD_STORE_(&b->last_present_ns, 0);
        GTA6RT_HUD_STORE_(&b->gpu_last_done_ns, 0);
        GTA6RT_HUD_STORE_(&b->frame_ns_max, 0);
        GTA6RT_HUD_STORE_(&b->latency_ns_max, 0);
    }
}

static inline void gta6rt_hud_raise_max_(uint64_t* p, uint64_t v) {
    uint64_t seen = GTA6RT_HUD_LOAD_(p);
    while (v > seen && !__atomic_compare_exchange_n(p, &seen, v, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

/* Once per presented frame, from the presenting thread: counts the frame and the interval since the previous one. */
static inline void gta6rt_hud_note_present(void) {
    gta6rt_hud_stats* b = gta6rt_hud_stats_block();
    if (!GTA6RT_HUD_LOAD_(&b->active)) return;
    const uint64_t now = gta6rt_hud_now_ns();
    const uint64_t prev = __atomic_exchange_n(&b->last_present_ns, now, __ATOMIC_RELAXED);
    GTA6RT_HUD_ADD_(&b->presents, 1);
    if (prev && now > prev) {
        const uint64_t gap = now - prev;
        GTA6RT_HUD_ADD_(&b->frame_intervals, 1);
        GTA6RT_HUD_ADD_(&b->frame_ns_sum, gap);
        gta6rt_hud_raise_max_(&b->frame_ns_max, gap);
    }
}

static inline void gta6rt_hud_add_draws(uint64_t n) {
    gta6rt_hud_stats* b = gta6rt_hud_stats_block();
    if (GTA6RT_HUD_LOAD_(&b->active)) GTA6RT_HUD_ADD_(&b->draws, n);
}

static inline void gta6rt_hud_add_dispatches(uint64_t n) {
    gta6rt_hud_stats* b = gta6rt_hud_stats_block();
    if (!GTA6RT_HUD_LOAD_(&b->active)) return;
    GTA6RT_HUD_ADD_(&b->dispatches, n);
    GTA6RT_HUD_OR_(&b->have, GTA6RT_HUD_HAVE_DISPATCHES);
}

/* One queue submission (ExecuteCommandLists): `cpu_ns` is how long the submitting thread was held. */
static inline void gta6rt_hud_note_submission(uint64_t cpu_ns) {
    gta6rt_hud_stats* b = gta6rt_hud_stats_block();
    if (!GTA6RT_HUD_LOAD_(&b->active)) return;
    GTA6RT_HUD_ADD_(&b->submissions, 1);
    GTA6RT_HUD_ADD_(&b->exec_cpu_ns, cpu_ns);
    GTA6RT_HUD_OR_(&b->have, GTA6RT_HUD_HAVE_SUBMISSIONS | GTA6RT_HUD_HAVE_EXEC_CPU);
}

/* GPU time of one unit of work (a command buffer, a pass): from timestamps or a completion handler. */
static inline void gta6rt_hud_note_gpu_ns(uint64_t ns) {
    gta6rt_hud_stats* b = gta6rt_hud_stats_block();
    if (!GTA6RT_HUD_LOAD_(&b->active)) return;
    GTA6RT_HUD_ADD_(&b->gpu_ns, ns);
    GTA6RT_HUD_ADD_(&b->gpu_samples, 1);
    GTA6RT_HUD_OR_(&b->have, GTA6RT_HUD_HAVE_GPU);
}

/* In-order queue without timestamps: busy time is the completion minus the later of the submit and the previous
 * completion. Call from the completion callback with the time the work was submitted. */
static inline void gta6rt_hud_note_gpu_done(uint64_t submit_ns) {
    gta6rt_hud_stats* b = gta6rt_hud_stats_block();
    if (!GTA6RT_HUD_LOAD_(&b->active)) return;
    const uint64_t done = gta6rt_hud_now_ns();
    const uint64_t last = __atomic_exchange_n(&b->gpu_last_done_ns, done, __ATOMIC_RELAXED);
    const uint64_t from = submit_ns > last ? submit_ns : last;
    if (done > from) gta6rt_hud_note_gpu_ns(done - from);
}

/* Present call to on-screen (presented handler), where the backend can supply it. */
static inline void gta6rt_hud_note_display_latency_ns(uint64_t ns) {
    gta6rt_hud_stats* b = gta6rt_hud_stats_block();
    if (!GTA6RT_HUD_LOAD_(&b->active)) return;
    GTA6RT_HUD_ADD_(&b->latency_ns_sum, ns);
    GTA6RT_HUD_ADD_(&b->latency_samples, 1);
    gta6rt_hud_raise_max_(&b->latency_ns_max, ns);
    GTA6RT_HUD_OR_(&b->have, GTA6RT_HUD_HAVE_LATENCY);
}

/* Running totals the producer already keeps (the vk-counters), stored as they are. */
static inline void gta6rt_hud_publish_totals(uint64_t draws, uint64_t dispatches, uint64_t submissions,
                                             uint64_t exec_cpu_ns) {
    gta6rt_hud_stats* b = gta6rt_hud_stats_block();
    if (!GTA6RT_HUD_LOAD_(&b->active)) return;
    GTA6RT_HUD_STORE_(&b->draws, draws);
    GTA6RT_HUD_STORE_(&b->dispatches, dispatches);
    GTA6RT_HUD_STORE_(&b->submissions, submissions);
    GTA6RT_HUD_STORE_(&b->exec_cpu_ns, exec_cpu_ns);
    GTA6RT_HUD_OR_(&b->have, GTA6RT_HUD_HAVE_SUBMISSIONS | GTA6RT_HUD_HAVE_EXEC_CPU | GTA6RT_HUD_HAVE_DISPATCHES);
}

static inline void gta6rt_hud_publish_pipelines(uint64_t built, uint64_t cache_hits, uint64_t pso_failures) {
    gta6rt_hud_stats* b = gta6rt_hud_stats_block();
    if (!GTA6RT_HUD_LOAD_(&b->active)) return;
    GTA6RT_HUD_STORE_(&b->pipelines_built, built);
    GTA6RT_HUD_STORE_(&b->pipeline_cache_hits, cache_hits);
    GTA6RT_HUD_STORE_(&b->pso_failures, pso_failures);
    GTA6RT_HUD_OR_(&b->have, GTA6RT_HUD_HAVE_PIPELINES);
}

/* Gauge: pipelines queued or being built by the compiler right now. */
static inline void gta6rt_hud_set_pipelines_pending(uint64_t n) {
    gta6rt_hud_stats* b = gta6rt_hud_stats_block();
    if (!GTA6RT_HUD_LOAD_(&b->active)) return;
    GTA6RT_HUD_STORE_(&b->pipelines_pending, n);
    GTA6RT_HUD_OR_(&b->have, GTA6RT_HUD_HAVE_PIPELINES);
}

static inline void gta6rt_hud_publish_textures_waiting(uint64_t n) {
    gta6rt_hud_stats* b = gta6rt_hud_stats_block();
    if (!GTA6RT_HUD_LOAD_(&b->active)) return;
    GTA6RT_HUD_STORE_(&b->textures_waiting, n);
    GTA6RT_HUD_OR_(&b->have, GTA6RT_HUD_HAVE_TEXTURES);
}

static inline void gta6rt_hud_add_imports(uint64_t n) {
    gta6rt_hud_stats* b = gta6rt_hud_stats_block();
    if (!GTA6RT_HUD_LOAD_(&b->active)) return;
    GTA6RT_HUD_ADD_(&b->guest_imports, n);
    GTA6RT_HUD_OR_(&b->have, GTA6RT_HUD_HAVE_IMPORTS);
}

/* The host memory governor: process budget, GPU bytes against the GPU's working-set limit, its level (0..3) and
 * whether memory is about to run out (warning != 0). Stored whether or not the overlay is active. */
static inline void gta6rt_hud_publish_memory(uint64_t budget, uint64_t gpu_bytes, uint64_t gpu_limit, uint64_t level,
                                             uint64_t warning) {
    gta6rt_hud_stats* b = gta6rt_hud_stats_block();
    GTA6RT_HUD_STORE_(&b->mem_budget, budget);
    GTA6RT_HUD_STORE_(&b->gpu_bytes, gpu_bytes);
    GTA6RT_HUD_STORE_(&b->gpu_limit, gpu_limit);
    GTA6RT_HUD_STORE_(&b->governor_level, level);
    GTA6RT_HUD_STORE_(&b->mem_warning, warning);
    GTA6RT_HUD_OR_(&b->have, GTA6RT_HUD_HAVE_MEMORY);
}

/* Short text, at most 63 bytes. Never blocks: when another thread is writing the same slot the update is dropped. */
static inline void gta6rt_hud_set_string(gta6rt_hud_string* s, const char* text) {
    uint32_t seq = __atomic_load_n(&s->seq, __ATOMIC_RELAXED);
    if ((seq & 1u) ||
        !__atomic_compare_exchange_n(&s->seq, &seq, seq + 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return;
    size_t n = text ? strlen(text) : 0;
    if (n >= GTA6RT_HUD_STRING_LEN) n = GTA6RT_HUD_STRING_LEN - 1;
    if (n) memcpy(s->text, text, n);
    s->text[n] = 0;
    __atomic_store_n(&s->seq, seq + 2, __ATOMIC_RELEASE);
}

/* Copies the slot into out[GTA6RT_HUD_STRING_LEN]; returns 0 when a writer kept it busy for every retry (out empty). */
static inline int gta6rt_hud_get_string(const gta6rt_hud_string* s, char* out) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t before = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
        if (before & 1u) continue;
        memcpy(out, s->text, GTA6RT_HUD_STRING_LEN);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&s->seq, __ATOMIC_RELAXED) == before) {
            out[GTA6RT_HUD_STRING_LEN - 1] = 0;
            return 1;
        }
    }
    out[0] = 0;
    return 0;
}

static inline void gta6rt_hud_set_state(const char* text) { gta6rt_hud_set_string(&gta6rt_hud_stats_block()->state, text); }
static inline void gta6rt_hud_set_driver(const char* text) { gta6rt_hud_set_string(&gta6rt_hud_stats_block()->driver, text); }
static inline void gta6rt_hud_set_profile(const char* text) {
    gta6rt_hud_set_string(&gta6rt_hud_stats_block()->profile, text);
}
static inline void gta6rt_hud_set_mem_note(const char* text) {
    gta6rt_hud_set_string(&gta6rt_hud_stats_block()->mem_note, text);
}

#ifdef __cplusplus
}
#endif

#endif
