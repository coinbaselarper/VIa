// The window layer with a real NSWindow, for a machine with a display session: aspect fit, full screen, minimise,
// the menu's key equivalents, focus/size/move messages. Opens one window for a few seconds. Skips (exit 0) when no
// display is available.
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "mac_window.h"
#include "shims.h"
#include "gui_helpers.h"
#include "stubs.h"
#include "win/appkit_window.h"
#include "win/win_core.h"
#include "window_shim.h"

using host::win::Msg;
namespace wm = host::win::wm;

namespace {
int g_failed = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok) ++g_failed;
}

struct WndClassExA {
    uint32_t cbSize, style;
    uint64_t lpfnWndProc;
    int32_t cbClsExtra, cbWndExtra;
    uint64_t hInstance, hIcon, hCursor, hbrBackground;
    const char* lpszMenuName;
    const char* lpszClassName;
    uint64_t hIconSm;
};
using RegisterFn = uint16_t(WINAPI*)(const WndClassExA*);
using CreateFn = uint64_t(WINAPI*)(uint32_t, const char*, const char*, uint32_t, int32_t, int32_t, int32_t, int32_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t);
using ShowFn = int(WINAPI*)(uint64_t, int);
using PeekFn = int(WINAPI*)(Msg*, uint64_t, uint32_t, uint32_t, uint32_t);
using DispatchFn = uint64_t(WINAPI*)(const Msg*);
using DefFn = uint64_t(WINAPI*)(uint64_t, uint32_t, uint64_t, uint64_t);
DefFn def_proc;
PeekFn peek;
DispatchFn dispatch;
ShowFn show_window;

uint64_t test_wndproc(uint64_t hwnd, uint64_t msg, uint64_t wp, uint64_t lp) {
    return def_proc(hwnd, static_cast<uint32_t>(msg), wp, lp);
}

std::vector<Msg> g_all;

void pump(double seconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        gta6rt_mac_window_pump_events(0.02);
        Msg m{};
        while (peek(&m, 0, 0, 0, 1)) g_all.push_back(m);
    }
}

template <class F>
bool pump_until(F pred, double seconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        pump(0.05);
        if (pred()) return true;
    }
    return pred();
}

const Msg* last_of(uint32_t message) {
    for (auto it = g_all.rbegin(); it != g_all.rend(); ++it)
        if (it->message == message) return &*it;
    return nullptr;
}

bool saw(uint32_t message, uint64_t wp, bool check_wp = true) {
    for (const Msg& m : g_all)
        if (m.message == message && (!check_wp || m.wParam == wp)) return true;
    return false;
}

}  // namespace

int main() {
    {
        if (!gui_has_display()) {
            std::printf("no display session: skipped\n");
            return 0;
        }
        auto reg = reinterpret_cast<RegisterFn>(rt::shims::find("user32.dll", "RegisterClassExA"));
        auto create = reinterpret_cast<CreateFn>(rt::shims::find("user32.dll", "CreateWindowExA"));
        show_window = reinterpret_cast<ShowFn>(rt::shims::find("user32.dll", "ShowWindow"));
        peek = reinterpret_cast<PeekFn>(rt::shims::find("user32.dll", "PeekMessageA"));
        dispatch = reinterpret_cast<DispatchFn>(rt::shims::find("user32.dll", "DispatchMessageA"));
        def_proc = reinterpret_cast<DefFn>(rt::shims::find("user32.dll", "DefWindowProcA"));
        host::window::set_hidden(false);

        host_win_screen screen{};
        check(host_win_screen_info(&screen) && screen.scale >= 1.0 && screen.width_px > 0, "the main screen answers");
        std::printf("screen: %ux%u px, scale %.1f, visible area %.0fx%.0f px, title bar %.0f px\n", screen.width_px,
                    screen.height_px, screen.scale, screen.area_w_px, screen.area_h_px, screen.chrome_h_px);

        WndClassExA wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = reinterpret_cast<uint64_t>(&test_wndproc);
        wc.lpszClassName = "sgaWindowClass";
        reg(&wc);
        const uint64_t hwnd = create(0, "sgaWindowClass", "Americas (test)", 0xcf0000, static_cast<int32_t>(0x80000000u),
                                     static_cast<int32_t>(0x80000000u), 3840, 2160, 0, 0, 0, 0);
        check(hwnd != 0, "CreateWindowExA returns a window");
        auto* win = reinterpret_cast<gta6rt_mac_window*>(hwnd);
        check(gta6rt_mac_window_visible(win), "it is a visible NSWindow");
        pump(0.5);

        uint32_t w = 0, h = 0;
        gta6rt_mac_window_client_pixels(win, &w, &h);
        std::printf("client: %ux%u\n", w, h);
        check(w > 0 && h > 0 && w <= 3840 && h <= 2160, "the client area fits what the title asked for");
        check(std::abs(double(w) / h - 16.0 / 9.0) < 0.01, "and keeps the title's 16:9");
        check(w <= screen.area_w_px + 1 && h <= screen.area_h_px - screen.chrome_h_px + 1, "and fits the screen's visible area");
        int32_t x = 0, y = 0;
        uint32_t cw = 0, ch = 0;
        check(host_win_client_rect(win, &x, &y, &cw, &ch) && cw == w && ch == h, "the AppKit client rect agrees with the layer");

        show_window(hwnd, 5);
        pump(0.5);
        check(saw(wm::ShowWindow, 1), "ShowWindow produced WM_SHOWWINDOW(TRUE)");
        std::printf("messages so far:");
        for (const Msg& m : g_all) std::printf(" %s", host::win::message_name(m.message));
        std::printf("\n");

        g_all.clear();
        check(host::window::set_fullscreen(true), "set_fullscreen(true) is accepted");
        const bool entered = pump_until([] { return host::window::fullscreen(); }, 8.0);
        check(entered, "the window enters full screen");
        pump(1.0);
        const Msg* big = last_of(wm::Size);
        check(big && big->wParam == host::win::kSizeMaximized, "WM_SIZE(SIZE_MAXIMIZED) reports it");
        uint32_t fw = 0, fh = 0;
        gta6rt_mac_window_client_pixels(win, &fw, &fh);
        std::printf("full screen client: %ux%u\n", fw, fh);
        check(fw >= w && fh >= h, "and the client area is larger");
        check(std::abs(double(fw) / fh - 16.0 / 9.0) < 0.01, "and still 16:9 (the rest of the screen is black)");
        check(host::window::set_fullscreen(false), "set_fullscreen(false) is accepted");
        check(pump_until([] { return !host::window::fullscreen(); }, 8.0), "the window leaves full screen");
        pump(1.0);
        check(last_of(wm::Size) && last_of(wm::Size)->wParam == host::win::kSizeRestored, "WM_SIZE(SIZE_RESTORED) reports it");

        g_all.clear();
        show_window(hwnd, 6);
        check(pump_until([] { return saw(wm::Size, host::win::kSizeMinimized); }, 5.0), "minimise gives WM_SIZE(SIZE_MINIMIZED)");
        g_all.clear();
        show_window(hwnd, 9);
        check(pump_until([] { return saw(wm::Size, host::win::kSizeRestored); }, 5.0), "restore gives WM_SIZE(SIZE_RESTORED)");
        pump(0.5);

        // The menu's key equivalents, through NSApp (no Accessibility permission needed).
        if (gui_make_key(win)) {
            pump(0.3);
            check(gui_menu_items() > 0, "the window menu was installed");
            gui_key(win, "f", 3, 1, 1);
            check(pump_until([] { return host::window::fullscreen(); }, 8.0), "Ctrl+Cmd+F toggles full screen on");
            pump(1.0);
            gui_key(win, "f", 3, 1, 1);
            check(pump_until([] { return !host::window::fullscreen(); }, 8.0), "and off again");
            pump(1.0);
            g_all.clear();
            gui_key(win, "F11", 103, 0, 0);
            const bool f11 = pump_until([] { return host::window::fullscreen(); }, 4.0);
            std::printf("F11 toggled full screen: %s\n", f11 ? "yes" : "no (not asserted)");
            if (f11) {
                host::window::set_fullscreen(false);
                pump_until([] { return !host::window::fullscreen(); }, 8.0);
                pump(1.0);
            }
            gui_key(win, "a", 0, 0, 0);
            pump(0.2);
            g_all.clear();
            gui_key(win, "q", 12, 0, 1);
            pump(0.3);
            const Msg* sys = last_of(wm::SysCommand);
            check(sys && (sys->wParam & 0xFFF0) == host::win::kScClose, "Cmd+Q queues WM_SYSCOMMAND(SC_CLOSE) instead of terminating");
            if (sys) {
                const int before = stubs::suspend_requests;
                dispatch(sys);
                check(stubs::suspend_requests == before + 1, "which DefWindowProcA turns into WM_CLOSE and the title's suspend");
            }
        } else {
            check(false, "the NSWindow behind the layer is found");
        }
        std::printf("posted %zu messages\n", host::window::stats().posted);
    }
    std::printf("%d failure(s)\n", g_failed);
    return g_failed ? 1 : 0;
}
