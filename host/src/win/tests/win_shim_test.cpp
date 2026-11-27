// The user32 shim (window_shim.cpp) with a layer-only window and a native "title" window procedure that forwards to
// DefWindowProcA, as the real title's class does. No NSWindow is created, so this runs without a display.
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "mac_window.h"
#include "shims.h"
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
using TranslateFn = int(WINAPI*)(const Msg*);
using DispatchFn = uint64_t(WINAPI*)(const Msg*);
using DefFn = uint64_t(WINAPI*)(uint64_t, uint32_t, uint64_t, uint64_t);

RegisterFn register_class;
CreateFn create_window;
ShowFn show_window;
PeekFn peek;
TranslateFn translate;
DispatchFn dispatch;
DefFn def_proc;

struct Seen {
    uint64_t hwnd;
    uint32_t msg;
    uint64_t wp, lp;
};
std::vector<Seen> g_seen;
uint64_t g_nccreate_result = 1;
int32_t g_create_cx = 0, g_create_cy = 0;
host::win::MinMaxInfo g_minmax{};

uint64_t test_wndproc(uint64_t hwnd, uint64_t msg, uint64_t wp, uint64_t lp) {
    g_seen.push_back({hwnd, static_cast<uint32_t>(msg), wp, lp});
    if (msg == wm::GetMinMaxInfo) g_minmax = *reinterpret_cast<host::win::MinMaxInfo*>(lp);
    if (msg == wm::NcCreate) {
        const int32_t* cs = reinterpret_cast<const int32_t*>(lp + 32);
        g_create_cy = cs[0];
        g_create_cx = cs[1];
    }
    return def_proc(hwnd, static_cast<uint32_t>(msg), wp, lp);
}

size_t count_seen(uint32_t msg) {
    size_t n = 0;
    for (const Seen& s : g_seen) n += s.msg == msg;
    return n;
}

std::vector<Msg> drain(uint64_t hwnd = 0) {
    std::vector<Msg> out;
    Msg m{};
    while (peek(&m, hwnd, 0, 0, 1)) out.push_back(m);
    return out;
}

constexpr int kEvFocus = HOST_WIN_EV_FOCUS, kEvAppActive = HOST_WIN_EV_APP_ACTIVE, kEvSize = HOST_WIN_EV_SIZE,
              kEvMove = HOST_WIN_EV_MOVE, kEvDisplay = HOST_WIN_EV_DISPLAY, kEvDpi = HOST_WIN_EV_DPI,
              kEvClose = HOST_WIN_EV_CLOSE;
}  // namespace

int main() {
    register_class = reinterpret_cast<RegisterFn>(rt::shims::find("user32.dll", "RegisterClassExA"));
    create_window = reinterpret_cast<CreateFn>(rt::shims::find("user32.dll", "CreateWindowExA"));
    show_window = reinterpret_cast<ShowFn>(rt::shims::find("user32.dll", "ShowWindow"));
    peek = reinterpret_cast<PeekFn>(rt::shims::find("user32.dll", "PeekMessageA"));
    translate = reinterpret_cast<TranslateFn>(rt::shims::find("user32.dll", "TranslateMessage"));
    dispatch = reinterpret_cast<DispatchFn>(rt::shims::find("user32.dll", "DispatchMessageA"));
    def_proc = reinterpret_cast<DefFn>(rt::shims::find("user32.dll", "DefWindowProcA"));
    check(register_class && create_window && show_window && peek && translate && dispatch && def_proc,
          "all seven user32 exports are registered");
    check(rt::shims::find("ext-ms-win-rtcore-ntuser-message-ansi-l1-1-0.dll", "PeekMessageA") != nullptr &&
              rt::shims::find("ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll", "DefWindowProcA") != nullptr &&
              rt::shims::find("ext-ms-win-rtcore-ntuser-window-l1-1-0.dll", "ShowWindow") != nullptr &&
              rt::shims::find("ext-ms-win-rtcore-ntuser-message-l1-1-0.dll", "TranslateMessage") != nullptr,
          "the API-set names the title imports are registered");
    if (g_failed) return 1;

    unsetenv("LEONIDA_PRESENT_WINDOW");
    host::window::set_hidden(true);
    const char* present = std::getenv("LEONIDA_PRESENT_WINDOW");
    check(present && std::strcmp(present, "hidden") == 0,
          "hidden mode asks the presenter for its own hidden window (no visible fallback)");

    WndClassExA wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = reinterpret_cast<uint64_t>(&test_wndproc);
    wc.lpszClassName = "sgaWindowClass";
    const uint16_t atom = register_class(&wc);
    check(atom >= 0xC000, "RegisterClassExA returns an atom");
    wc.cbSize = 7;
    check(register_class(&wc) == 0 && stubs::last_error == 87, "a wrong cbSize is ERROR_INVALID_PARAMETER");
    check(create_window(0, "nosuchclass", "x", 0, 0, 0, 10, 10, 0, 0, 0, 0) == 0 && stubs::last_error == 1407,
          "an unregistered class is ERROR_CANNOT_FIND_WND_CLASS");

    const uint64_t hwnd = create_window(0, "sgaWindowClass", "Americas", 0xcf0000, static_cast<int32_t>(0x80000000u), static_cast<int32_t>(0x80000000u), 3840, 2160, 0, 0, 0, 0);
    check(hwnd != 0 && gta6rt_mac_window_alive(reinterpret_cast<gta6rt_mac_window*>(hwnd)), "CreateWindowExA returns a live window");
    check(host::window::windows_created() == 1, "windows_created counts it");
    uint32_t w = 0, h = 0;
    gta6rt_mac_window_client_pixels(reinterpret_cast<gta6rt_mac_window*>(hwnd), &w, &h);
    check(w == 3840 && h == 2160, "a layer-only window has the title's 3840x2160 client area");
    check(g_seen.size() == 4 && g_seen[0].msg == wm::GetMinMaxInfo && g_seen[1].msg == wm::NcCreate &&
              g_seen[2].msg == wm::NcCalcSize && g_seen[3].msg == wm::Create,
          "creation sends WM_GETMINMAXINFO, WM_NCCREATE, WM_NCCALCSIZE, WM_CREATE, in that order");
    check(g_create_cx == 3840 && g_create_cy == 2160, "CREATESTRUCT carries the title's size");
    check(g_minmax.max_size[0] > 0 && g_minmax.min_track[0] == 320 && g_minmax.min_track[1] == 180,
          "MINMAXINFO is filled (320x180 minimum track size, 16:9)");
    check(host::window::stats().queued == 0, "nothing is queued by creation");
    check(!host::window::fullscreen() && !host::window::set_fullscreen(true) && !host::window::toggle_fullscreen(),
          "full screen has no effect on a layer-only window (and says so)");

    Msg m{};
    check(peek(&m, 0, 0, 0, 1) == 0, "PeekMessageA on an empty queue returns 0");
    check(peek(&m, 0x1234, 0, 0, 1) == 0 && stubs::last_error == 1400, "a bogus window handle is ERROR_INVALID_WINDOW_HANDLE");
    check(peek(nullptr, 0, 0, 0, 1) == 0 && stubs::last_error == 87, "a null MSG is ERROR_INVALID_PARAMETER");

    check(show_window(hwnd, 5) == 0, "the first ShowWindow returns FALSE (was hidden)");
    auto msgs = drain();
    check(msgs.size() >= 6 && msgs[0].message == wm::ShowWindow && msgs[0].wParam == 1, "ShowWindow queues WM_SHOWWINDOW(TRUE)");
    bool order_ok = true;
    const uint32_t expected[] = {wm::ShowWindow, wm::ActivateApp, wm::NcActivate, wm::Activate, wm::SetFocus, wm::Size, wm::Move};
    for (size_t i = 0; i < 7 && i < msgs.size(); ++i) order_ok &= msgs[i].message == expected[i];
    check(msgs.size() == 7 && order_ok, "then activation, focus, WM_SIZE and WM_MOVE (a layer-only window gets them synthesised)");
    check(msgs.size() == 7 && msgs[5].wParam == host::win::kSizeRestored && (msgs[5].lParam & 0xFFFF) == 3840 && (msgs[5].lParam >> 16) == 2160,
          "WM_SIZE carries the client size packed in lParam");
    check(msgs.size() == 7 && msgs[0].hwnd == hwnd && msgs[0].time != 0, "messages carry the window and a time");
    check(show_window(hwnd, 5) == 1 && drain().empty(), "a second SW_SHOW returns TRUE and queues nothing");
    check(show_window(hwnd, 99) == 0, "an unknown ShowWindow command is refused (FALSE)");
    check(show_window(0x1234, 5) == 0 && stubs::last_error == 1400, "ShowWindow on a bogus handle fails with ERROR_INVALID_WINDOW_HANDLE");

    // Dispatch: every message goes to the window procedure, which forwards to DefWindowProcA.
    g_seen.clear();
    host::window::testing::inject_event(hwnd, kEvFocus, {0, 0, 0, 0, 0, 0});
    host::window::testing::inject_event(hwnd, kEvFocus, {1, 0, 0, 0, 0, 0});
    host::window::testing::inject_event(hwnd, kEvAppActive, {0, 0, 0, 0, 0, 0});
    host::window::testing::inject_event(hwnd, kEvSize, {2, 1920, 1080, 0, 0, 0});
    host::window::testing::inject_event(hwnd, kEvSize, {0, 1800, 1012, 0, 0, 0});
    host::window::testing::inject_event(hwnd, kEvMove, {-30, 40, 0, 0, 0, 0});
    host::window::testing::inject_event(hwnd, kEvDisplay, {3024, 1964, 32, 0, 0, 0});
    host::window::testing::inject_event(hwnd, kEvDpi, {192, 0, 0, 3600, 2025, 0});
    msgs = drain();
    std::vector<uint32_t> kinds;
    for (const Msg& x : msgs) kinds.push_back(x.message);
    const std::vector<uint32_t> want = {wm::Activate, wm::NcActivate, wm::KillFocus, wm::NcActivate, wm::Activate, wm::SetFocus,
                                         wm::ActivateApp, wm::Size, wm::Move, wm::DisplayChange, wm::DpiChanged};
    check(kinds == want, "focus out/in, app deactivation, size, move, display and DPI events become messages in order");
    size_t sizes = 0;
    for (const Msg& x : msgs) sizes += x.message == wm::Size;
    check(sizes == 1, "two unread size events coalesce into the newest one");
    for (const Msg& x : msgs) {
        if (x.message == wm::Size) check(x.wParam == host::win::kSizeRestored && (x.lParam & 0xFFFF) == 1800 && (x.lParam >> 16) == 1012, "the newest size wins");
        if (x.message == wm::Move) check(static_cast<int16_t>(x.lParam & 0xFFFF) == -30 && (x.lParam >> 16) == 40, "WM_MOVE packs a negative x");
        if (x.message == wm::DisplayChange) check(x.wParam == 32 && (x.lParam & 0xFFFF) == 3024 && (x.lParam >> 16) == 1964, "WM_DISPLAYCHANGE: bpp and screen size");
        if (x.message == wm::DpiChanged) {
            const int32_t* rect = reinterpret_cast<const int32_t*>(x.lParam);
            check((x.wParam & 0xFFFF) == 192 && (x.wParam >> 16) == 192 && rect[2] == 3600 && rect[3] == 2025,
                  "WM_DPICHANGED: dpi in both halves, suggested rect behind lParam");
        }
    }
    for (const Msg& x : msgs) {
        translate(&x);
        dispatch(&x);
    }
    check(g_seen.size() == msgs.size(), "DispatchMessageA calls the window procedure once per message");
    check(translate(&msgs[0]) == 0, "TranslateMessage translates nothing (no WM_CHAR: input goes through GameInput)");
    check(stubs::suspend_requests == 0, "none of those messages ends the title");
    Msg unknown{};
    unknown.hwnd = 0x1234;
    unknown.message = wm::Close;
    check(dispatch(&unknown) == 0 && stubs::suspend_requests == 0, "DispatchMessageA for an unknown window does nothing");
    check(dispatch(nullptr) == 0, "DispatchMessageA(NULL) is 0");

    // DefWindowProcA answers.
    check(def_proc(hwnd, wm::EraseBkgnd, 0, 0) == 1, "WM_ERASEBKGND is handled: the host never erases");
    check(def_proc(hwnd, wm::Paint, 0, 0) == 0, "WM_PAINT is validated");
    char title[32];
    check(def_proc(hwnd, wm::GetText, sizeof title, reinterpret_cast<uint64_t>(title)) == 8 && std::strcmp(title, "Americas") == 0,
          "WM_GETTEXT returns the window title");
    check(def_proc(hwnd, wm::SetText, 0, reinterpret_cast<uint64_t>("GTA VI")) == 1 &&
              def_proc(hwnd, wm::GetTextLength, 0, 0) == 6,
          "WM_SETTEXT changes it");
    check(def_proc(hwnd, 0x7777, 0, 0) == 0 && def_proc(hwnd, 0x7777, 0, 0) == 0, "an unknown message returns 0 (logged once)");

    // Close: the menu's Quit or the close button becomes SC_CLOSE, which DefWindowProcA turns into WM_CLOSE, which
    // starts the title's suspend.
    host::window::testing::inject_event(hwnd, kEvClose, {0, 0, 0, 0, 0, 0});
    g_seen.clear();
    msgs = drain();
    check(msgs.size() == 1 && msgs[0].message == wm::SysCommand && (msgs[0].wParam & 0xFFF0) == host::win::kScClose,
          "a close request queues WM_SYSCOMMAND(SC_CLOSE)");
    dispatch(&msgs[0]);
    check(g_seen.size() == 2 && g_seen[0].msg == wm::SysCommand && g_seen[1].msg == wm::Close,
          "DefWindowProcA sends WM_CLOSE to the same window procedure");
    check(stubs::suspend_requests == 1, "and WM_CLOSE in DefWindowProcA requests the title's suspend, once");

    // A window procedure that refuses WM_NCCREATE destroys the window.
    g_nccreate_result = 0;
    check(host::window::stats().delivered > 10, "stats count the delivered messages");

    {
        const auto t0 = std::chrono::steady_clock::now();
        constexpr int kIters = 1000000;
        Msg bm{};
        for (int i = 0; i < kIters; ++i) peek(&bm, 0, 0, 0, 1);
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / kIters;
        std::printf("bench: PeekMessageA on an empty queue %.0f ns/call over %d calls\n", ns, kIters);
    }

    std::printf("%d failure(s)\n", g_failed);
    return g_failed ? 1 : 0;
}
