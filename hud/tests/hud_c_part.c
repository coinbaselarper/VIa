#include "gta6rt_hud/stats.h"

gta6rt_hud_stats* hud_c_block(void) { return gta6rt_hud_stats_block(); }

void hud_c_produce(unsigned frames) {
    for (unsigned i = 0; i < frames; ++i) {
        gta6rt_hud_add_draws(3);
        gta6rt_hud_note_present();
    }
}
