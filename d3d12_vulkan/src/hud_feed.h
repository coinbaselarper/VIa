#pragma once
// port/hud: feeds the run counters (vk_run_counters.h) into the performance overlay's stats block. Inactive (one
// relaxed load) unless GTA6RT_HUD / --hud switched the overlay on.

#include <gta6rt_hud/stats.h>

#include "vk_run_counters.h"

namespace leonida::hud_feed {

inline void publish_run_counters() {
    if (!gta6rt_hud_active()) return;
    const auto load = [](const std::atomic<std::uint64_t>& a) { return a.load(std::memory_order_relaxed); };
    const run_counters::Counters& c = run_counters::counters();
    gta6rt_hud_publish_totals(load(c.draws_emitted), load(c.dispatches_emitted),
                              load(c.submissions_ok) + load(c.submissions_failed), load(c.submit_wall_us) * 1000);
    gta6rt_hud_publish_pipelines(load(c.vk_graphics_built) + load(c.vk_compute_built), load(c.vk_cache_hits),
                                 load(c.pso_graphics_failed) + load(c.pso_compute_failed));
}

}  // namespace leonida::hud_feed
