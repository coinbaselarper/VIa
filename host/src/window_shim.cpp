// The title's user32 surface: RegisterClassExA, CreateWindowExA, ShowWindow, PeekMessageA, TranslateMessage,
// DispatchMessageA, DefWindowProcA. An HWND is a gta6rt_mac_window* (d3d12_vulkan presents to it).
//
// What the title does (exe+0x24784d5..0x2478553, message loop at exe+0x24847a8): it registers "sgaWindowClass" with
// DefWindowProcA itself as the window procedure (the registered address is the exe's import thunk for it), creates a
// 3840x2160 WS_OVERLAPPEDWINDOW "Americas", calls ShowWindow, and then drains PeekMessageA(NULL, 0, 0, PM_REMOVE)
// into TranslateMessage/DispatchMessageA without ever looking at a message. So every message is answered by
// DefWindowProcA, and the messages that matter are the ones DefWindowProcA acts on: the close request, which here
// starts the title's console suspend (lifecycle) instead of DestroyWindow.
//
// Messages come from three places: synchronously at creation (WM_GETMINMAXINFO, WM_NCCREATE, WM_NCCALCSIZE,
// WM_CREATE), from AppKit through win/appkit_window.mm (activation, focus, size, move, screen, scale, full screen,
// the menu's Quit) and from the close button. WM_PAINT and WM_ERASEBKGND are never generated: the layer is presented
// by Vulkan, and DefWindowProcA answers them as a window with nothing to repaint.
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include <gta6rt/os.h>
#include <gta6rt/shims.h>

#include "mac_window.h"
#include "shims.h"
#include "win/appkit_window.h"
#include "win/win_core.h"
#include "window_shim.h"

namespace host::window {
namespace {

using host::win::Msg;
namespace wm = host::win::wm;

constexpr int32_t kCwUseDefault = static_cast<int32_t>(0x80000000u);

struct WndClassExA {
    uint32_t cbSize;
    uint32_t style;
    uint64_t lpfnWndProc;
    int32_t cbClsExtra;
    int32_t cbWndExtra;
    uint64_t hInstance, hIcon, hCursor, hbrBackground;
    const char* lpszMenuName;
    const char* lpszClassName;
    uint64_t hIconSm;
};
static_assert(sizeof(WndClassExA) == 80, "WNDCLASSEXA");

struct CreateStructA {
    uint64_t lpCreateParams, hInstance, hMenu, hwndParent;
    int32_t cy, cx, y, x;
    int32_t style;
    uint32_t pad;
    const char* lpszName;
    const char* lpszClass;
    uint32_t dwExStyle;
    uint32_t pad2;
};
static_assert(sizeof(CreateStructA) == 80, "CREATESTRUCTA");

struct Window {
    uint64_t wndproc = 0;
    std::string cls;
    std::string title;
    uint32_t logical_w = 0, logical_h = 0;
    bool attached = false;
    bool shown = false;
    bool close_posted = false;
};

std::mutex g_mu;
std::map<std::string, uint64_t> g_classes;
std::map<uint64_t, Window> g_windows;
std::atomic<uint16_t> g_next_atom{0xC000};
std::atomic<bool> g_hidden{false};
std::atomic<int> g_created{0};
host::win::MessageQueue g_queue;
std::set<uint32_t> g_unhandled_logged;
std::atomic<uint64_t> g_delivered{0};

std::string lower(const char* s) {
    std::string r;
    for (; s && *s; ++s) r.push_back(static_cast<char>(*s >= 'A' && *s <= 'Z' ? *s + 32 : *s));
    return r;
}

std::string class_key(const char* name) {
    if (reinterpret_cast<uintptr_t>(name) < 0x10000) return "#" + std::to_string(reinterpret_cast<uintptr_t>(name));
    return lower(name);
}

uint32_t now_ms() { return static_cast<uint32_t>(gta6rt_time_monotonic_ns() / 1000000); }

uint64_t call_wndproc(uint64_t proc, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp) {
    const uint64_t args[4] = {hwnd, msg, wp, lp};
    return gta6rt_os_invoke(proc, args, 4);
}

void post(uint64_t hwnd, uint32_t message, uint64_t wp, uint64_t lp) {
    Msg m{};
    m.hwnd = hwnd;
    m.message = message;
    m.wParam = wp;
    m.lParam = lp;
    m.time = now_ms();
    g_queue.post(m);
}

uint64_t window_proc_of(uint64_t hwnd) {
    std::lock_guard lock(g_mu);
    auto it = g_windows.find(hwnd);
    return it == g_windows.end() ? 0 : it->second.wndproc;
}

uint64_t WINAPI def_window_proc_a(uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp);

uint64_t send_message(uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp) {
    const uint64_t proc = window_proc_of(hwnd);
    if (!proc) return 0;
    return call_wndproc(proc, hwnd, msg, wp, lp);
}

void apply_show(uint64_t hwnd, int cmd);

class ShimDefHost final : public host::win::DefHost {
public:
    bool window_exists(uint64_t hwnd) override {
        std::lock_guard lock(g_mu);
        return g_windows.count(hwnd) != 0;
    }
    std::string title(uint64_t hwnd) override {
        std::lock_guard lock(g_mu);
        auto it = g_windows.find(hwnd);
        return it == g_windows.end() ? std::string() : it->second.title;
    }
    void set_title(uint64_t hwnd, const std::string& title) override {
        {
            std::lock_guard lock(g_mu);
            auto it = g_windows.find(hwnd);
            if (it == g_windows.end()) return;
            it->second.title = title;
        }
        gta6rt_mac_window_set_title(reinterpret_cast<gta6rt_mac_window*>(hwnd), title.c_str());
    }
    void request_close(uint64_t hwnd) override {
        gta6rt_log(GTA6RT_LOG_INFO, "window", "WM_CLOSE reached DefWindowProcA for %#llx: the title's suspend starts",
                   static_cast<unsigned long long>(hwnd));
        gta6rt_shims_request_suspend();
    }
    uint64_t send_message(uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp) override {
        return ::host::window::send_message(hwnd, msg, wp, lp);
    }
    void show(uint64_t hwnd, int cmd) override { apply_show(hwnd, cmd); }
    void log_unhandled(uint64_t hwnd, uint32_t msg) override {
        {
            std::lock_guard lock(g_mu);
            if (!g_unhandled_logged.insert(msg).second) return;
        }
        gta6rt_log(GTA6RT_LOG_WARN, "window", "DefWindowProcA: message %#x for %#llx has no specific handling (returns 0)", msg,
                   static_cast<unsigned long long>(hwnd));
    }
};

ShimDefHost g_def_host;

uint64_t WINAPI def_window_proc_a(uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp) {
    return host::win::def_window_proc(g_def_host, hwnd, msg, wp, lp);
}

void on_host_event(void*, gta6rt_mac_window* window, const host_win_event* ev) {
    const uint64_t hwnd = reinterpret_cast<uint64_t>(window);
    const auto& v = ev->v;
    static const char* const kNames[] = {"focus", "app active", "size", "move", "display", "dpi", "close", "full screen"};
    gta6rt_log(GTA6RT_LOG_INFO, "window", "event %s (%d, %d, %d) for %#llx", kNames[ev->kind], v[0], v[1], v[2],
               static_cast<unsigned long long>(hwnd));
    switch (ev->kind) {
        case HOST_WIN_EV_FOCUS:
            if (v[0]) {
                post(hwnd, wm::NcActivate, 1, 0);
                post(hwnd, wm::Activate, host::win::kWaActive, 0);
                post(hwnd, wm::SetFocus, 0, 0);
            } else {
                post(hwnd, wm::Activate, host::win::kWaInactive, 0);
                post(hwnd, wm::NcActivate, 0, 0);
                post(hwnd, wm::KillFocus, 0, 0);
            }
            break;
        case HOST_WIN_EV_APP_ACTIVE:
            post(hwnd, wm::ActivateApp, v[0] ? 1 : 0, 0);
            break;
        case HOST_WIN_EV_SIZE:
            post(hwnd, wm::Size, static_cast<uint32_t>(v[0]), host::win::make_lparam(v[1], v[2]));
            break;
        case HOST_WIN_EV_MOVE:
            post(hwnd, wm::Move, 0, host::win::make_lparam(v[0], v[1]));
            break;
        case HOST_WIN_EV_DISPLAY:
            post(hwnd, wm::DisplayChange, static_cast<uint32_t>(v[2]), host::win::make_lparam(v[0], v[1]));
            break;
        case HOST_WIN_EV_DPI: {
            static int32_t rects[8][4];
            static std::atomic<uint32_t> next{0};
            int32_t* rect = rects[next.fetch_add(1) % 8];
            for (int i = 0; i < 4; ++i) rect[i] = v[1 + i];
            const uint32_t dpi = static_cast<uint32_t>(v[0]);
            post(hwnd, wm::DpiChanged, host::win::make_lparam(static_cast<int32_t>(dpi), static_cast<int32_t>(dpi)),
                 reinterpret_cast<uint64_t>(rect));
            break;
        }
        case HOST_WIN_EV_CLOSE:
            post(hwnd, wm::SysCommand, host::win::kScClose, 0);
            break;
        case HOST_WIN_EV_FULLSCREEN:
            gta6rt_log(GTA6RT_LOG_INFO, "window", "%s full screen", v[0] ? "entered" : "left");
            break;
    }
}

void poll_close(uint64_t only) {
    std::lock_guard lock(g_mu);
    for (auto& [h, rec] : g_windows) {
        if (only && h != only) continue;
        if (rec.close_posted) continue;
        if (gta6rt_mac_window_close_requested(reinterpret_cast<const gta6rt_mac_window*>(h))) {
            rec.close_posted = true;
            post(h, wm::SysCommand, host::win::kScClose, 0);
        }
    }
}

bool is_minimize_cmd(int cmd) { return cmd == 2 || cmd == 6 || cmd == 7; }

void apply_show(uint64_t hwnd, int cmd) {
    auto* win = reinterpret_cast<gta6rt_mac_window*>(hwnd);
    bool attached = false, changed = false, now_shown = false;
    {
        std::lock_guard lock(g_mu);
        auto it = g_windows.find(hwnd);
        if (it == g_windows.end()) return;
        attached = it->second.attached;
        now_shown = cmd != 0;
        changed = it->second.shown != now_shown;
        it->second.shown = now_shown;
    }
    gta6rt_log(GTA6RT_LOG_INFO, "window", "ShowWindow(%#llx, %d)%s", static_cast<unsigned long long>(hwnd), cmd,
               attached ? "" : " (layer only: no AppKit window to change)");
    if (attached) {
        host_win_show(win, cmd);
        if (changed) post(hwnd, wm::ShowWindow, now_shown ? 1 : 0, 0);
        return;
    }
    if (!changed) return;
    post(hwnd, wm::ShowWindow, now_shown ? 1 : 0, 0);
    if (now_shown && !is_minimize_cmd(cmd)) {
        uint32_t w = 0, h = 0;
        gta6rt_mac_window_client_pixels(win, &w, &h);
        post(hwnd, wm::ActivateApp, 1, 0);
        post(hwnd, wm::NcActivate, 1, 0);
        post(hwnd, wm::Activate, host::win::kWaActive, 0);
        post(hwnd, wm::SetFocus, 0, 0);
        post(hwnd, wm::Size, host::win::kSizeRestored, host::win::make_lparam(static_cast<int32_t>(w), static_cast<int32_t>(h)));
        post(hwnd, wm::Move, 0, 0);
    }
}

uint16_t WINAPI register_class_ex_a(const WndClassExA* wc) {
    if (!wc || wc->cbSize != sizeof(WndClassExA) || !wc->lpszClassName) {
        gta6rt_last_error_set(87);
        return 0;
    }
    const uint16_t atom = g_next_atom.fetch_add(1);
    std::lock_guard lock(g_mu);
    g_classes[class_key(wc->lpszClassName)] = wc->lpfnWndProc;
    g_classes["#" + std::to_string(atom)] = wc->lpfnWndProc;
    gta6rt_log(GTA6RT_LOG_INFO, "window", "RegisterClassExA(\"%s\") -> atom %#x, wndproc %#llx",
               reinterpret_cast<uintptr_t>(wc->lpszClassName) < 0x10000 ? "#atom" : wc->lpszClassName, atom,
               static_cast<unsigned long long>(wc->lpfnWndProc));
    return atom;
}

void destroy_failed(uint64_t hwnd, gta6rt_mac_window* win, bool attached) {
    {
        std::lock_guard lock(g_mu);
        g_windows.erase(hwnd);
    }
    g_queue.drop_window(hwnd);
    if (attached) host_win_detach(win);
    gta6rt_mac_window_destroy(win);
}

uint64_t WINAPI create_window_ex_a(uint32_t ex_style, const char* cls, const char* title, uint32_t style, int32_t x, int32_t y,
                                   int32_t w, int32_t h, uint64_t parent, uint64_t menu, uint64_t instance, uint64_t param) {
    uint64_t proc = 0;
    {
        std::lock_guard lock(g_mu);
        auto it = g_classes.find(class_key(cls));
        if (it == g_classes.end()) {
            gta6rt_last_error_set(1407);
            return 0;
        }
        proc = it->second;
    }
    const uint32_t cw = w == kCwUseDefault || w <= 0 ? 1920u : static_cast<uint32_t>(w);
    const uint32_t ch = h == kCwUseDefault || h <= 0 ? 1080u : static_cast<uint32_t>(h);
    const bool visible = !g_hidden.load();
    uint32_t pw = cw, ph = ch;
    if (visible && !host_win_plan_size(cw, ch, &pw, &ph))
        gta6rt_log(GTA6RT_LOG_WARN, "window", "the main screen did not answer; the window keeps the title's %ux%u", cw, ch);
    const char* name = title ? title : "Grand Theft Auto VI";
    gta6rt_mac_window* win = gta6rt_mac_window_create(name, pw, ph, visible);
    if (!win) {
        gta6rt_last_error_set(8);
        return 0;
    }
    const uint64_t hwnd = reinterpret_cast<uint64_t>(win);
    const bool attached = visible && gta6rt_mac_window_visible(win) && host_win_attach(win, cw, ch, on_host_event, nullptr);
    {
        std::lock_guard lock(g_mu);
        Window& rec = g_windows[hwnd];
        rec.wndproc = proc;
        rec.cls = class_key(cls);
        rec.title = name;
        rec.logical_w = cw;
        rec.logical_h = ch;
        rec.attached = attached;
    }
    g_created.fetch_add(1);
    uint32_t aw = 0, ah = 0;
    gta6rt_mac_window_client_pixels(win, &aw, &ah);
    gta6rt_log(GTA6RT_LOG_INFO, "window",
               "CreateWindowExA(\"%s\", %ux%u, style %#x, ex %#x) -> hwnd %#llx (%s, client %ux%u)", name, cw, ch, style, ex_style,
               static_cast<unsigned long long>(hwnd),
               !visible ? "layer only" : attached ? "visible" : "visible, no NSWindow found", aw, ah);

    CreateStructA cs{};
    cs.lpCreateParams = param;
    cs.hInstance = instance;
    cs.hMenu = menu;
    cs.hwndParent = parent;
    cs.cx = static_cast<int32_t>(cw);
    cs.cy = static_cast<int32_t>(ch);
    cs.x = x == kCwUseDefault ? 0 : x;
    cs.y = y == kCwUseDefault ? 0 : y;
    cs.style = static_cast<int32_t>(style);
    cs.lpszName = title;
    cs.lpszClass = cls;
    cs.dwExStyle = ex_style;
    const uint64_t lp = reinterpret_cast<uint64_t>(&cs);

    host_win_screen screen{};
    if (visible) host_win_screen_info(&screen);
    host::win::MinMaxInfo mmi{};
    const int32_t sw = screen.width_px ? static_cast<int32_t>(screen.width_px) : static_cast<int32_t>(cw);
    const int32_t sh = screen.height_px ? static_cast<int32_t>(screen.height_px) : static_cast<int32_t>(ch);
    mmi.max_size[0] = mmi.max_track[0] = sw;
    mmi.max_size[1] = mmi.max_track[1] = sh;
    mmi.min_track[0] = 320;
    mmi.min_track[1] = static_cast<int32_t>(320.0 * ch / cw);
    call_wndproc(proc, hwnd, wm::GetMinMaxInfo, 0, reinterpret_cast<uint64_t>(&mmi));
    if (!call_wndproc(proc, hwnd, wm::NcCreate, 0, lp)) {
        gta6rt_log(GTA6RT_LOG_WARN, "window", "WM_NCCREATE returned FALSE: window destroyed");
        destroy_failed(hwnd, win, attached);
        return 0;
    }
    int32_t rect[4] = {cs.x, cs.y, cs.x + static_cast<int32_t>(cw), cs.y + static_cast<int32_t>(ch)};
    call_wndproc(proc, hwnd, wm::NcCalcSize, 0, reinterpret_cast<uint64_t>(rect));
    if (static_cast<int64_t>(call_wndproc(proc, hwnd, wm::Create, 0, lp)) == -1) {
        gta6rt_log(GTA6RT_LOG_WARN, "window", "WM_CREATE returned -1: window destroyed");
        destroy_failed(hwnd, win, attached);
        return 0;
    }
    if (attached) {
        const char* fs = std::getenv("GTA6RT_FULLSCREEN");
        if (fs && *fs && fs[0] != '0') host_win_set_fullscreen(win, 1);
    }
    return hwnd;
}

int WINAPI show_window(uint64_t hwnd, int cmd) {
    const auto* win = reinterpret_cast<const gta6rt_mac_window*>(hwnd);
    if (!gta6rt_mac_window_alive(win)) {
        gta6rt_last_error_set(1400);
        return 0;
    }
    bool was = false;
    {
        std::lock_guard lock(g_mu);
        auto it = g_windows.find(hwnd);
        if (it == g_windows.end()) {
            gta6rt_last_error_set(1400);
            return 0;
        }
        was = it->second.shown;
    }
    if (cmd < 0 || cmd > 10) {
        gta6rt_log(GTA6RT_LOG_WARN, "window", "ShowWindow(%#llx, %d): unknown command, ignored (returns FALSE)",
                   static_cast<unsigned long long>(hwnd), cmd);
        return 0;
    }
    apply_show(hwnd, cmd);
    return was ? 1 : 0;
}

int WINAPI peek_message_a(Msg* msg, uint64_t hwnd, uint32_t min, uint32_t max, uint32_t remove) {
    if (!msg) {
        gta6rt_last_error_set(87);
        return 0;
    }
    if (hwnd && !gta6rt_mac_window_alive(reinterpret_cast<const gta6rt_mac_window*>(hwnd))) {
        gta6rt_last_error_set(1400);
        return 0;
    }
    poll_close(hwnd);
    if (!g_queue.peek(msg, hwnd, min, max, (remove & host::win::kPmRemove) != 0)) return 0;
    return 1;
}

int WINAPI translate_message(const Msg*) { return 0; }

uint64_t WINAPI dispatch_message_a(const Msg* msg) {
    if (!msg || !msg->hwnd) return 0;
    const uint64_t proc = window_proc_of(msg->hwnd);
    if (!proc) return 0;
    g_delivered.fetch_add(1);
    return call_wndproc(proc, msg->hwnd, msg->message, msg->wParam, msg->lParam);
}

}  // namespace

void set_hidden(bool hidden) {
    g_hidden.store(hidden);
    if (hidden) setenv("LEONIDA_PRESENT_WINDOW", "hidden", 0);
}

int windows_created() { return g_created.load(); }

bool set_fullscreen(bool on) {
    std::lock_guard lock(g_mu);
    for (auto& [h, rec] : g_windows) {
        if (!rec.attached) continue;
        return host_win_set_fullscreen(reinterpret_cast<gta6rt_mac_window*>(h), on ? 1 : 0) != 0;
    }
    return false;
}

bool toggle_fullscreen() {
    std::lock_guard lock(g_mu);
    for (auto& [h, rec] : g_windows) {
        if (!rec.attached) continue;
        auto* win = reinterpret_cast<gta6rt_mac_window*>(h);
        return host_win_set_fullscreen(win, host_win_is_fullscreen(win) ? 0 : 1) != 0;
    }
    return false;
}

bool fullscreen() {
    std::lock_guard lock(g_mu);
    for (auto& [h, rec] : g_windows)
        if (rec.attached) return host_win_is_fullscreen(reinterpret_cast<const gta6rt_mac_window*>(h)) != 0;
    return false;
}

namespace testing {
void inject_event(uint64_t hwnd, int kind, const int32_t (&v)[6]) {
    host_win_event ev{static_cast<host_win_event_kind>(kind), {v[0], v[1], v[2], v[3], v[4], v[5]}};
    on_host_event(nullptr, reinterpret_cast<gta6rt_mac_window*>(hwnd), &ev);
}
}  // namespace testing

Stats stats() {
    Stats s;
    s.posted = g_queue.posted();
    s.dropped = g_queue.dropped();
    s.queued = g_queue.size();
    s.delivered = g_delivered.load();
    return s;
}

}  // namespace host::window

namespace {
using namespace host::window;
}  // namespace

SHIM_TABLE(host_user32,
    SHIM("user32.dll", "RegisterClassExA", register_class_ex_a),
    SHIM("user32.dll", "CreateWindowExA", create_window_ex_a),
    SHIM("user32.dll", "ShowWindow", show_window),
    SHIM("user32.dll", "PeekMessageA", peek_message_a),
    SHIM("user32.dll", "TranslateMessage", translate_message),
    SHIM("user32.dll", "DispatchMessageA", dispatch_message_a),
    SHIM("user32.dll", "DefWindowProcA", def_window_proc_a),
    SHIM("ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll", "RegisterClassExA", register_class_ex_a),
    SHIM("ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll", "CreateWindowExA", create_window_ex_a),
    SHIM("ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll", "DefWindowProcA", def_window_proc_a),
    SHIM("ext-ms-win-rtcore-ntuser-window-l1-1-0.dll", "ShowWindow", show_window),
    SHIM("ext-ms-win-rtcore-ntuser-message-ansi-l1-1-0.dll", "PeekMessageA", peek_message_a),
    SHIM("ext-ms-win-rtcore-ntuser-message-ansi-l1-1-0.dll", "DispatchMessageA", dispatch_message_a),
    SHIM("ext-ms-win-rtcore-ntuser-message-l1-1-0.dll", "TranslateMessage", translate_message),
);
