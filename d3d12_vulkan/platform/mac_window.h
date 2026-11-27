#pragma once

/*
 * Windows for presentation on the macOS host: an NSWindow whose content view is backed by a CAMetalLayer, the
 * surface VK_EXT_metal_surface takes. Plain C, no windows.h (metal_window.mm is Objective-C++).
 *
 * On this host an HWND that d3d12_vulkan presents to is a gta6rt_mac_window*. Windows are kept in a process-wide
 * registry: rt::d3d12::find_title_window picks the title's window from it, so whatever serves the title's
 * CreateWindowExW (a user32 shim, not this module) should create its windows with gta6rt_mac_window_create.
 *
 * Threads: AppKit objects belong to the main thread. A visible window is created there (directly when called on
 * the main thread, else through the main dispatch queue, which the host's main thread must be servicing; after
 * main_wait_ms without an answer the call falls back to a layer-only window and logs it). A hidden window is only a
 * CAMetalLayer, which any thread may create and present to.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gta6rt_mac_window gta6rt_mac_window;

gta6rt_mac_window* gta6rt_mac_window_create(const char* title_utf8, uint32_t width, uint32_t height, int visible);
void gta6rt_mac_window_destroy(gta6rt_mac_window* window);
/* Nonzero while `window` is a live window of the registry (IsWindow). */
int gta6rt_mac_window_alive(const gta6rt_mac_window* window);
/* The CAMetalLayer* for VkMetalSurfaceCreateInfoEXT::pLayer. */
void* gta6rt_mac_window_metal_layer(gta6rt_mac_window* window);
/* Client area in pixels (the layer's drawable size), and in points. 0 when the window is gone or minimised. */
int gta6rt_mac_window_client_pixels(const gta6rt_mac_window* window, uint32_t* width, uint32_t* height);
int gta6rt_mac_window_visible(const gta6rt_mac_window* window);
void gta6rt_mac_window_set_title(gta6rt_mac_window* window, const char* title_utf8);
void gta6rt_mac_window_set_client_size(gta6rt_mac_window* window, uint32_t width, uint32_t height);
/* Set when the user closed the window (the red button); the window itself stays until destroyed. */
int gta6rt_mac_window_close_requested(const gta6rt_mac_window* window);
/* The visible window with the largest client area other than `exclude`, or null (find_title_window). */
gta6rt_mac_window* gta6rt_mac_window_find_largest(const gta6rt_mac_window* exclude);
/* Main thread only: handle pending AppKit events for up to timeout_seconds. Returns the number handled. */
int gta6rt_mac_window_pump_events(double timeout_seconds);
/* How long a visible create from another thread waits for the main queue (default 5000 ms). */
void gta6rt_mac_window_set_main_wait_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif
