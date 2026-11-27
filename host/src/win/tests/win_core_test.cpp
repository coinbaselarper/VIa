// win_core.h: the message queue, DefWindowProcA's answers and the client-size fit. No window, no AppKit.
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../win_core.h"

using namespace host::win;

namespace {
int g_failed = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok) ++g_failed;
}

struct FakeHost final : DefHost {
    std::map<uint64_t, std::string> titles;
    std::vector<std::pair<uint32_t, uint64_t>> sent;
    std::vector<int> shows;
    int closes = 0;
    int unhandled = 0;
    bool window_exists(uint64_t h) override { return titles.count(h) != 0; }
    std::string title(uint64_t h) override { return titles[h]; }
    void set_title(uint64_t h, const std::string& t) override { titles[h] = t; }
    void request_close(uint64_t) override { ++closes; }
    uint64_t send_message(uint64_t h, uint32_t m, uint64_t, uint64_t) override {
        sent.push_back({m, h});
        return 0;
    }
    void show(uint64_t, int cmd) override { shows.push_back(cmd); }
    void log_unhandled(uint64_t, uint32_t) override { ++unhandled; }
};

Msg msg(uint64_t hwnd, uint32_t m, uint64_t wp = 0, uint64_t lp = 0) {
    Msg x{};
    x.hwnd = hwnd;
    x.message = m;
    x.wParam = wp;
    x.lParam = lp;
    return x;
}
}  // namespace

int main() {
    // ---- the queue
    MessageQueue q;
    Msg out{};
    check(!q.peek(&out, 0, 0, 0, true), "an empty queue has nothing to peek");
    q.post(msg(1, wm::Activate, 1));
    q.post(msg(2, wm::SetFocus));
    q.post(msg(1, wm::Close));
    check(q.size() == 3, "three messages queued");
    check(q.peek(&out, 2, 0, 0, false) && out.message == wm::SetFocus && q.size() == 3, "a window filter finds its own, PM_NOREMOVE keeps it");
    check(q.peek(&out, 0, wm::Close, wm::Close, true) && out.hwnd == 1 && q.size() == 2, "a range filter finds WM_CLOSE and removes it");
    check(q.peek(&out, 0, 0, 0, true) && out.message == wm::Activate && out.wParam == 1, "the rest come in order");
    check(q.peek(&out, 0, 0, 0, true) && out.message == wm::SetFocus && !q.peek(&out, 0, 0, 0, true), "and the queue drains");

    q.post(msg(1, wm::Size, kSizeRestored, make_lparam(100, 50)));
    q.post(msg(1, wm::Activate, 1));
    q.post(msg(1, wm::Size, kSizeMaximized, make_lparam(1920, 1080)));
    q.post(msg(2, wm::Size, kSizeRestored, make_lparam(7, 7)));
    check(q.size() == 3, "WM_SIZE replaces an unread WM_SIZE of the same window only");
    q.peek(&out, 1, wm::Size, wm::Size, true);
    check(out.wParam == kSizeMaximized && (out.lParam & 0xFFFF) == 1920 && (out.lParam >> 16) == 1080, "the newest size is the one kept");
    for (int i = 0; i < 600; ++i) q.post(msg(9, wm::Close, i));
    check(q.size() == MessageQueue::kCapacity && q.dropped() > 0, "a full queue drops its oldest and counts it");
    q.drop_window(9);
    check(q.size() <= 2, "drop_window removes a window's messages");
    check(make_lparam(-1, -2) == (0xFFFFull | (0xFFFEull << 16)), "make_lparam packs negative coordinates as 16-bit halves");

    // ---- DefWindowProcA
    FakeHost h;
    h.titles[5] = "Americas";
    auto def = [&](uint32_t m, uint64_t wp = 0, uint64_t lp = 0) { return def_window_proc(h, 5, m, wp, lp); };
    check(def(wm::NcCreate) == 1 && def(wm::Create) == 0, "WM_NCCREATE is TRUE, WM_CREATE 0");
    check(def(wm::EraseBkgnd) == 1, "WM_ERASEBKGND is handled (nothing to erase)");
    check(def(wm::Paint) == 0 && def(wm::NcPaint) == 0, "WM_PAINT and WM_NCPAINT are validated");
    check(def(wm::NcHitTest) == 1 && def(wm::MouseActivate) == 1 && def(wm::NcActivate) == 1, "hit test is client, activate is MA_ACTIVATE");
    check(def(wm::QueryOpen) == 1 && def(wm::QueryEndSession) == 1 && def(wm::EndSession) == 0, "session and open queries");
    for (uint32_t m : {wm::Activate, wm::ActivateApp, wm::SetFocus, wm::KillFocus, wm::Size, wm::Move, wm::ShowWindow,
                       wm::DisplayChange, wm::DpiChanged, wm::GetMinMaxInfo, wm::Destroy, wm::NcDestroy})
        def(m);
    check(h.unhandled == 0 && h.closes == 0, "the messages the host generates are all known to DefWindowProcA");
    check(def(0x9999) == 0 && h.unhandled == 1, "an unknown message returns 0 and is reported");
    def(wm::Close);
    check(h.closes == 1, "WM_CLOSE asks the host to close (the title's suspend)");
    def(wm::SysCommand, kScClose | 0xF, 0);
    check(h.sent.size() == 1 && h.sent[0].first == wm::Close && h.sent[0].second == 5, "SC_CLOSE sends WM_CLOSE to the window");
    def(wm::SysCommand, kScMinimize);
    def(wm::SysCommand, kScMaximize);
    def(wm::SysCommand, kScRestore);
    check(h.shows == std::vector<int>({6, 3, 9}), "SC_MINIMIZE, SC_MAXIMIZE and SC_RESTORE show the window");
    check(def(wm::GetTextLength) == 8, "WM_GETTEXTLENGTH");
    char buf[16];
    check(def(wm::GetText, 16, reinterpret_cast<uint64_t>(buf)) == 8 && std::strcmp(buf, "Americas") == 0, "WM_GETTEXT copies the title");
    check(def(wm::GetText, 4, reinterpret_cast<uint64_t>(buf)) == 3 && std::strcmp(buf, "Ame") == 0, "WM_GETTEXT truncates and terminates");
    check(def(wm::SetText, 0, reinterpret_cast<uint64_t>("GTA VI")) == 1 && h.titles[5] == "GTA VI", "WM_SETTEXT sets it");
    check(def_window_proc(h, 77, wm::GetTextLength, 0, 0) == 0 && def_window_proc(h, 77, wm::Close, 0, 0) == 0 && h.closes == 1,
          "a window that does not exist closes nothing");

    // ---- the client-size fit
    Size s = fit_client(3840, 2160, 7680, 4320, 60);
    check(s.w == 3840 && s.h == 2160, "a 3840x2160 title on a larger screen keeps its size");
    s = fit_client(3840, 2160, 3600, 2112, 56);
    check(s.w == 3600 && s.h == 2025, "on the 1800x1056 point (3600x2112 px) screen with a 56 px title bar: 3600x2025, 16:9");
    s = fit_client(3840, 2160, 2000, 3000, 0);
    check(s.w == 2000 && s.h == 1125, "a narrow screen is limited by width");
    s = fit_client(3840, 2160, 5000, 1000, 100);
    check(s.h == 900 && s.w == 1600, "a short screen is limited by height after the title bar");
    s = fit_client(3840, 2160, 0, 0, 0);
    check(s.w == 3840 && s.h == 2160, "an unknown screen keeps the title's size");
    s = fit_client(0, 100, 100, 100, 0);
    check(s.w == 0 && s.h == 0, "an empty request stays empty");

    std::printf("%d failure(s)\n", g_failed);
    return g_failed ? 1 : 0;
}
