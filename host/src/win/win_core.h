#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace host::win {

namespace wm {
constexpr uint32_t Create = 0x0001, Destroy = 0x0002, Move = 0x0003, Size = 0x0005, Activate = 0x0006,
                   SetFocus = 0x0007, KillFocus = 0x0008, SetText = 0x000C, GetText = 0x000D, GetTextLength = 0x000E,
                   Paint = 0x000F, Close = 0x0010, QueryEndSession = 0x0011, QueryOpen = 0x0013,
                   EraseBkgnd = 0x0014, EndSession = 0x0016, ShowWindow = 0x0018, ActivateApp = 0x001C,
                   SetCursor = 0x0020, MouseActivate = 0x0021, GetMinMaxInfo = 0x0024, WindowPosChanging = 0x0046,
                   WindowPosChanged = 0x0047, DisplayChange = 0x007E, NcCreate = 0x0081, NcDestroy = 0x0082,
                   NcCalcSize = 0x0083, NcHitTest = 0x0084, NcPaint = 0x0085, NcActivate = 0x0086,
                   SysCommand = 0x0112, DeviceChange = 0x0219, DpiChanged = 0x02E0;
}

constexpr uint32_t kScClose = 0xF060, kScMinimize = 0xF020, kScMaximize = 0xF030, kScRestore = 0xF120;
constexpr uint32_t kSizeRestored = 0, kSizeMinimized = 1, kSizeMaximized = 2;
constexpr uint32_t kWaInactive = 0, kWaActive = 1;
constexpr uint32_t kPmRemove = 1;

struct Msg {
    uint64_t hwnd;
    uint32_t message;
    uint32_t pad;
    uint64_t wParam, lParam;
    uint32_t time;
    int32_t x, y;
    uint32_t lPrivate;
};
static_assert(sizeof(Msg) == 48, "MSG");

struct MinMaxInfo {
    int32_t reserved[2], max_size[2], max_position[2], min_track[2], max_track[2];
};
static_assert(sizeof(MinMaxInfo) == 40, "MINMAXINFO");

constexpr uint64_t make_lparam(int32_t lo, int32_t hi) {
    return static_cast<uint32_t>(static_cast<uint16_t>(lo)) | (static_cast<uint64_t>(static_cast<uint16_t>(hi)) << 16);
}

// A bounded FIFO of posted messages for every window. WM_SIZE, WM_MOVE and WM_DISPLAYCHANGE replace an unread one of
// the same window (the newest geometry is all a reader needs). A full queue drops its oldest message and counts it.
class MessageQueue {
public:
    static constexpr size_t kCapacity = 512;

    void post(const Msg& m);
    // The first queued message whose window and range match (hwnd 0: any window; min == max == 0: any message).
    bool peek(Msg* out, uint64_t hwnd, uint32_t min, uint32_t max, bool remove);
    void drop_window(uint64_t hwnd);
    size_t size();
    size_t dropped();
    size_t posted();

private:
    std::mutex mu_;
    std::deque<Msg> q_;
    size_t dropped_ = 0, posted_ = 0;
};

// What DefWindowProcA needs from the host: the window's title and the few actions its messages ask for.
class DefHost {
public:
    virtual ~DefHost() = default;
    virtual bool window_exists(uint64_t hwnd) = 0;
    virtual std::string title(uint64_t hwnd) = 0;
    virtual void set_title(uint64_t hwnd, const std::string& title) = 0;
    virtual void request_close(uint64_t hwnd) = 0;
    virtual uint64_t send_message(uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp) = 0;
    virtual void show(uint64_t hwnd, int cmd) = 0;
    virtual void log_unhandled(uint64_t hwnd, uint32_t msg) = 0;
};

// DefWindowProcA. Messages this host never generates (WM_PAINT, WM_ERASEBKGND, the NC paint family) are answered as a
// window with no background and nothing to repaint would: erase handled, paint validated.
uint64_t def_window_proc(DefHost& host, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp);

// The client size, in pixels, the window takes on a screen: the title's size when it fits, else the same aspect
// scaled down to fit (never up). `chrome_h` is the title bar's height in the same unit as the area.
struct Size {
    uint32_t w = 0, h = 0;
};
Size fit_client(uint32_t want_w, uint32_t want_h, double area_w, double area_h, double chrome_h);

const char* message_name(uint32_t msg);

}  // namespace host::win
