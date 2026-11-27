// UCRT time imports. The guest's `struct tm` is nine ints (36 bytes); the host's has two more fields, so every call
// converts. __time64_t is 64-bit on both.
#include <sys/time.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>

#include "shims.h"

namespace rt::crt {
namespace {

struct WinTm {
    int32_t tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
};
static_assert(sizeof(WinTm) == 36);

struct WinTimeb64 {
    int64_t time;
    uint16_t millitm;
    int16_t timezone;
    int16_t dstflag;
};
static_assert(sizeof(WinTimeb64) == 16);

void to_win(const tm& t, WinTm* w) {
    w->tm_sec = t.tm_sec; w->tm_min = t.tm_min; w->tm_hour = t.tm_hour; w->tm_mday = t.tm_mday; w->tm_mon = t.tm_mon;
    w->tm_year = t.tm_year; w->tm_wday = t.tm_wday; w->tm_yday = t.tm_yday; w->tm_isdst = t.tm_isdst;
}

tm to_host(const WinTm& w) {
    tm t;
    std::memset(&t, 0, sizeof t);
    t.tm_sec = w.tm_sec; t.tm_min = w.tm_min; t.tm_hour = w.tm_hour; t.tm_mday = w.tm_mday; t.tm_mon = w.tm_mon;
    t.tm_year = w.tm_year; t.tm_wday = w.tm_wday; t.tm_yday = w.tm_yday; t.tm_isdst = w.tm_isdst;
    return t;
}

// The UCRT limits __time64_t to years 1970..3000; negative and larger values fail with EINVAL.
constexpr int64_t kMaxTime64 = 32535215999ll;

int64_t WINAPI w_time64(int64_t* out) {
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    if (out) *out = now;
    return now;
}

double WINAPI w_difftime64(int64_t a, int64_t b) { return static_cast<double>(a - b); }

void WINAPI w_ftime64(WinTimeb64* out) {
    timeval tv;
    gettimeofday(&tv, nullptr);
    tm local;
    const time_t sec = tv.tv_sec;
    localtime_r(&sec, &local);
    out->time = tv.tv_sec;
    out->millitm = static_cast<uint16_t>(tv.tv_usec / 1000);
    out->timezone = static_cast<int16_t>(-local.tm_gmtoff / 60);
    out->dstflag = static_cast<int16_t>(local.tm_isdst > 0 ? 1 : 0);
}

int WINAPI w_gmtime64_s(WinTm* out, const int64_t* t) {
    if (!out) return EINVAL;
    std::memset(out, 0xff, sizeof *out);
    if (!t || *t < 0 || *t > kMaxTime64) return EINVAL;
    tm r;
    const time_t sec = static_cast<time_t>(*t);
    gmtime_r(&sec, &r);
    to_win(r, out);
    return 0;
}

int WINAPI w_localtime64_s(WinTm* out, const int64_t* t) {
    if (!out) return EINVAL;
    std::memset(out, 0xff, sizeof *out);
    if (!t || *t < 0 || *t > kMaxTime64) return EINVAL;
    tm r;
    const time_t sec = static_cast<time_t>(*t);
    localtime_r(&sec, &r);
    to_win(r, out);
    return 0;
}

WinTm* WINAPI w_gmtime64(const int64_t* t) {
    static thread_local WinTm buf;
    if (w_gmtime64_s(&buf, t) != 0) {
        errno = EINVAL;
        return nullptr;
    }
    return &buf;
}

int64_t WINAPI w_mkgmtime64(WinTm* w) {
    if (!w) return -1;
    tm t = to_host(*w);
    const time_t r = ::timegm(&t);
    to_win(t, w);
    return (r < 0 || r > kMaxTime64) ? -1 : static_cast<int64_t>(r);
}

int64_t WINAPI w_mktime64(WinTm* w) {
    if (!w) return -1;
    tm t = to_host(*w);
    const time_t r = ::mktime(&t);
    to_win(t, w);
    return (r < 0 || r > kMaxTime64) ? -1 : static_cast<int64_t>(r);
}

size_t WINAPI w_strftime(char* buf, size_t size, const char* fmt, const WinTm* w) {
    if (!buf || !size || !fmt || !w) {
        errno = EINVAL;
        return 0;
    }
    tm t = to_host(*w);
    return ::strftime(buf, size, fmt, &t);
}

}  // namespace
}  // namespace rt::crt

namespace {
using namespace rt::crt;
}

SHIM_TABLE(crt_time,
    SHIM("ucrtbase.dll", "_time64", w_time64), SHIM("ucrtbase.dll", "_difftime64", w_difftime64),
    SHIM("ucrtbase.dll", "_ftime64", w_ftime64), SHIM("ucrtbase.dll", "_gmtime64", w_gmtime64),
    SHIM("ucrtbase.dll", "_gmtime64_s", w_gmtime64_s), SHIM("ucrtbase.dll", "_localtime64_s", w_localtime64_s),
    SHIM("ucrtbase.dll", "_mkgmtime64", w_mkgmtime64), SHIM("ucrtbase.dll", "_mktime64", w_mktime64),
    SHIM("ucrtbase.dll", "strftime", w_strftime),
);
