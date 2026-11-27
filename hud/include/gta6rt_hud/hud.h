#ifndef GTA6RT_HUD_H
#define GTA6RT_HUD_H

/*
 * Performance overlay: a separate click-through child window of the game's NSWindow (or the same text in the window
 * title), fed by the stats block in stats.h. Every call may come from any thread; AppKit work is queued to the main
 * thread and runs when the main run loop services it. Nothing is created before gta6rt_hud_attach, and an overlay that
 * fails to come up disables itself with one log line.
 *
 * Keys (see gta6rt_hud_handle_key): F1 cycles off, min, full, title; F2 toggles off and the last mode shown.
 */

#include <stdint.h>

#include "stats.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GTA6RT_HUD_API_VERSION 1

typedef enum gta6rt_hud_mode {
    GTA6RT_HUD_OFF = 0,
    GTA6RT_HUD_MIN = 1,
    GTA6RT_HUD_FULL = 2,
    GTA6RT_HUD_TITLE = 3
} gta6rt_hud_mode;

typedef struct gta6rt_hud_config {
    gta6rt_hud_mode mode;
    double rate_hz;
    double budget_ms;
} gta6rt_hud_config;

typedef struct gta6rt_hud_info {
    gta6rt_hud_mode mode;
    int attached;
    int overlay_visible;
    int timer_running;
    int disabled;
    double rate_hz;
    uint64_t ticks;
    uint64_t tick_ns_total;
    uint64_t tick_ns_max;
    uint64_t title_sets;
    uint64_t title_set_ns_total;
    uint64_t title_set_ns_max;
    uint64_t overlay_updates;
    uint64_t tick_interval_ns_total;
    uint64_t tick_interval_ns_max;
    uint64_t tick_interval_count;
    int memory_pressure;
    uint32_t overlay_window_number;
    char line[160];
} gta6rt_hud_info;

#define GTA6RT_HUD_KEY_F1 0x7A
#define GTA6RT_HUD_KEY_F2 0x78

int gta6rt_hud_parse_mode(const char* text, gta6rt_hud_mode* out);
const char* gta6rt_hud_mode_name(gta6rt_hud_mode mode);

void gta6rt_hud_init(const gta6rt_hud_config* config);
void gta6rt_hud_init_from_env(void);

int gta6rt_hud_attach(void* ns_window);
void gta6rt_hud_detach(void* ns_window);

void gta6rt_hud_set_mode(gta6rt_hud_mode mode);
gta6rt_hud_mode gta6rt_hud_get_mode(void);
void gta6rt_hud_set_rate(double hz);
void gta6rt_hud_set_import_counter(uint64_t (*fn)(void* ctx), void* ctx);

/* Main thread, from the key monitor: mac_keycode is NSEvent.keyCode. Returns 1 when the key was the HUD's. */
int gta6rt_hud_handle_key(unsigned short mac_keycode, unsigned long modifier_flags, int is_repeat);

void gta6rt_hud_get_info(gta6rt_hud_info* out);
void gta6rt_hud_shutdown(void);

void gta6rt_hud_test_set_pressure(int level);
void gta6rt_hud_test_reset_counters(void);

#ifdef __cplusplus
}
#endif

#endif
