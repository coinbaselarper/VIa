#include "win_core.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace host::win {

void MessageQueue::post(const Msg& m) {
    std::lock_guard lock(mu_);
    ++posted_;
    if (m.message == wm::Size || m.message == wm::Move || m.message == wm::DisplayChange) {
        for (auto it = q_.rbegin(); it != q_.rend(); ++it) {
            if (it->hwnd == m.hwnd && it->message == m.message) {
                *it = m;
                return;
            }
        }
    }
    if (q_.size() >= kCapacity) {
        q_.pop_front();
        ++dropped_;
    }
    q_.push_back(m);
}

bool MessageQueue::peek(Msg* out, uint64_t hwnd, uint32_t min, uint32_t max, bool remove) {
    std::lock_guard lock(mu_);
    for (auto it = q_.begin(); it != q_.end(); ++it) {
        if (hwnd && it->hwnd != hwnd) continue;
        if ((min || max) && (it->message < min || it->message > max)) continue;
        if (out) *out = *it;
        if (remove) q_.erase(it);
        return true;
    }
    return false;
}

void MessageQueue::drop_window(uint64_t hwnd) {
    std::lock_guard lock(mu_);
    q_.erase(std::remove_if(q_.begin(), q_.end(), [&](const Msg& m) { return m.hwnd == hwnd; }), q_.end());
}

size_t MessageQueue::size() {
    std::lock_guard lock(mu_);
    return q_.size();
}
size_t MessageQueue::dropped() {
    std::lock_guard lock(mu_);
    return dropped_;
}
size_t MessageQueue::posted() {
    std::lock_guard lock(mu_);
    return posted_;
}

uint64_t def_window_proc(DefHost& host, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp) {
    switch (msg) {
        case wm::NcCreate: return 1;
        case wm::Create: return 0;
        case wm::NcCalcSize: return 0;
        case wm::NcHitTest: return 1;  // HTCLIENT
        case wm::NcPaint: return 0;
        case wm::NcActivate: return 1;
        case wm::EraseBkgnd: return 1;
        case wm::Paint: return 0;
        case wm::MouseActivate: return 1;  // MA_ACTIVATE
        case wm::SetCursor: return 0;
        case wm::QueryOpen: return 1;
        case wm::QueryEndSession: return 1;
        case wm::EndSession: return 0;
        case wm::DeviceChange: return 1;
        case wm::Activate:
        case wm::ActivateApp:
        case wm::SetFocus:
        case wm::KillFocus:
        case wm::Size:
        case wm::Move:
        case wm::ShowWindow:
        case wm::WindowPosChanging:
        case wm::WindowPosChanged:
        case wm::GetMinMaxInfo:
        case wm::DisplayChange:
        case wm::DpiChanged:
        case wm::Destroy:
        case wm::NcDestroy: return 0;
        case wm::Close:
            if (host.window_exists(hwnd)) host.request_close(hwnd);
            return 0;
        case wm::SysCommand:
            switch (static_cast<uint32_t>(wp) & 0xFFF0u) {
                case kScClose: host.send_message(hwnd, wm::Close, 0, 0); return 0;
                case kScMinimize: host.show(hwnd, 6); return 0;
                case kScMaximize: host.show(hwnd, 3); return 0;
                case kScRestore: host.show(hwnd, 9); return 0;
                default: return 0;
            }
        case wm::GetTextLength: return host.window_exists(hwnd) ? host.title(hwnd).size() : 0;
        case wm::GetText: {
            auto* out = reinterpret_cast<char*>(lp);
            if (!out || wp == 0 || !host.window_exists(hwnd)) return 0;
            const std::string t = host.title(hwnd);
            const size_t n = std::min<size_t>(t.size(), static_cast<size_t>(wp) - 1);
            std::memcpy(out, t.data(), n);
            out[n] = '\0';
            return n;
        }
        case wm::SetText: {
            if (!host.window_exists(hwnd)) return 0;
            host.set_title(hwnd, lp ? std::string(reinterpret_cast<const char*>(lp)) : std::string());
            return 1;
        }
        default:
            host.log_unhandled(hwnd, msg);
            return 0;
    }
}

Size fit_client(uint32_t want_w, uint32_t want_h, double area_w, double area_h, double chrome_h) {
    if (!want_w || !want_h) return {};
    const double avail_h = area_h - chrome_h;
    if (area_w <= 0 || avail_h <= 0) return {want_w, want_h};
    const double s = std::min({1.0, area_w / want_w, avail_h / want_h});
    return {std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(want_w * s))),
            std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(want_h * s)))};
}

const char* message_name(uint32_t msg) {
    switch (msg) {
        case wm::Create: return "WM_CREATE";
        case wm::Destroy: return "WM_DESTROY";
        case wm::Move: return "WM_MOVE";
        case wm::Size: return "WM_SIZE";
        case wm::Activate: return "WM_ACTIVATE";
        case wm::SetFocus: return "WM_SETFOCUS";
        case wm::KillFocus: return "WM_KILLFOCUS";
        case wm::Paint: return "WM_PAINT";
        case wm::Close: return "WM_CLOSE";
        case wm::EraseBkgnd: return "WM_ERASEBKGND";
        case wm::ShowWindow: return "WM_SHOWWINDOW";
        case wm::ActivateApp: return "WM_ACTIVATEAPP";
        case wm::GetMinMaxInfo: return "WM_GETMINMAXINFO";
        case wm::DisplayChange: return "WM_DISPLAYCHANGE";
        case wm::NcCreate: return "WM_NCCREATE";
        case wm::NcDestroy: return "WM_NCDESTROY";
        case wm::NcCalcSize: return "WM_NCCALCSIZE";
        case wm::NcActivate: return "WM_NCACTIVATE";
        case wm::SysCommand: return "WM_SYSCOMMAND";
        case wm::DpiChanged: return "WM_DPICHANGED";
        default: return "WM_?";
    }
}

}  // namespace host::win
