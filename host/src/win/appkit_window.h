#pragma once
/*
 * AppKit side of the host's windows: what gta6rt_mac_window (d3d12_vulkan/platform/mac_window.h) leaves out. It finds
 * the NSWindow behind a visible gta6rt_mac_window, locks its aspect ratio, makes it a full-screen primary, observes
 * its focus, size, move, screen and backing-scale changes and reports them through a callback (on the main thread),
 * and installs one menu (Toggle Full Screen, Minimize, Quit) and a key filter so that typing in the window neither
 * beeps nor reaches AppKit (the game reads the keyboard through GameInput). Plain C, no windows.h.
 *
 * Threads: every call may come from any thread; the AppKit work runs on the main thread, which has to service the
 * main queue (the host's main loop does). Calls that need an answer wait up to 5 s for it.
 */
#include <stdint.h>

#include "mac_window.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum host_win_event_kind {
    HOST_WIN_EV_FOCUS,       /* v[0] = 1 key window gained, 0 lost */
    HOST_WIN_EV_APP_ACTIVE,  /* v[0] = 1 application became active, 0 resigned */
    HOST_WIN_EV_SIZE,        /* v[0] = 0 restored, 1 minimised, 2 full screen or zoomed; v[1], v[2] = client pixels */
    HOST_WIN_EV_MOVE,        /* v[0], v[1] = client origin in screen pixels, top-left origin */
    HOST_WIN_EV_DISPLAY,     /* v[0], v[1] = screen pixels, v[2] = bits per pixel */
    HOST_WIN_EV_DPI,         /* v[0] = dpi (96 x scale), v[1..4] = suggested left, top, right, bottom (screen pixels) */
    HOST_WIN_EV_CLOSE,       /* the menu's Quit */
    HOST_WIN_EV_FULLSCREEN   /* v[0] = 1 entered, 0 left */
} host_win_event_kind;

typedef struct host_win_event {
    host_win_event_kind kind;
    int32_t v[6];
} host_win_event;

typedef void (*host_win_event_fn)(void* ctx, gta6rt_mac_window* window, const host_win_event* event);

typedef struct host_win_screen {
    double scale;             /* backing scale factor of the main screen */
    uint32_t width_px, height_px;
    double area_w_px, area_h_px;  /* the screen's visible frame, in pixels */
    double chrome_h_px;           /* title bar height of a window with the host's style, in pixels */
} host_win_screen;

/* The main screen's geometry; 0 when the main thread did not answer. */
int host_win_screen_info(host_win_screen* out);

/* The window size (client pixels) the title's want_w x want_h takes on the main screen: the same when it fits, else
 * the 16:9 (or whatever aspect it asked for) shrunk to fit. Returns 0 and the wanted size when AppKit did not answer. */
int host_win_plan_size(uint32_t want_w, uint32_t want_h, uint32_t* out_w, uint32_t* out_h);

/* Attaches to a visible window; `logical_w` x `logical_h` fixes its aspect ratio. 0 when there is no NSWindow. */
int host_win_attach(gta6rt_mac_window* window, uint32_t logical_w, uint32_t logical_h, host_win_event_fn fn, void* ctx);
void host_win_detach(gta6rt_mac_window* window);

/* ShowWindow commands (SW_HIDE 0, SW_SHOWNORMAL 1, SW_SHOWMINIMIZED 2, SW_SHOWMAXIMIZED 3, SW_SHOWNOACTIVATE 4, SW_SHOW 5,
 * SW_MINIMIZE 6, SW_SHOWMINNOACTIVE 7, SW_SHOWNA 8, SW_RESTORE 9, SW_SHOWDEFAULT 10). Returns 0 for a command it does
 * not know. The change is made on the main thread after this returns. */
int host_win_show(gta6rt_mac_window* window, int sw_command);

int host_win_set_fullscreen(gta6rt_mac_window* window, int on);
int host_win_is_fullscreen(const gta6rt_mac_window* window);

/* The client size and screen position now, read from the NSWindow; 0 when not attached. */
int host_win_client_rect(const gta6rt_mac_window* window, int32_t* x, int32_t* y, uint32_t* w, uint32_t* h);

#ifdef __cplusplus
}
#endif
