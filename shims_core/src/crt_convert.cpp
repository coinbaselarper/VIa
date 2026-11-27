// UCRT conversion, utility, heap and locale imports. Windows `long` is 32-bit: strtol/strtoul/atoi/lroundf clamp to
// 32 bits and set ERANGE where the host's 64-bit `long` would not.
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <climits>

#include <gta6rt/isa.h>

#include "shims.h"
#include "util.h"

namespace rt::crt {
namespace {

int32_t WINAPI w_strtol(const char* s, char** end, int base) {
    errno = 0;
    const long long v = ::strtoll(s, end, base);
    if (v > INT32_MAX) {
        errno = ERANGE;
        return INT32_MAX;
    }
    if (v < INT32_MIN) {
        errno = ERANGE;
        return INT32_MIN;
    }
    return static_cast<int32_t>(v);
}

uint32_t WINAPI w_strtoul(const char* s, char** end, int base) {
    const char* p = s;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) ++p;
    bool neg = false;
    if (*p == '-' || *p == '+') {
        neg = *p == '-';
        ++p;
    }
    errno = 0;
    const unsigned long long v = ::strtoull(p, end, base);
    if (end && *end == p) *end = const_cast<char*>(s);
    if (errno == ERANGE || v > UINT32_MAX) {
        errno = ERANGE;
        return UINT32_MAX;
    }
    return neg ? static_cast<uint32_t>(0u - static_cast<uint32_t>(v)) : static_cast<uint32_t>(v);
}

int64_t WINAPI w_strtoll(const char* s, char** end, int base) { return ::strtoll(s, end, base); }
uint64_t WINAPI w_strtoull(const char* s, char** end, int base) { return ::strtoull(s, end, base); }
double WINAPI w_strtod(const char* s, char** end) { return ::strtod(s, end); }
float WINAPI w_strtof(const char* s, char** end) { return ::strtof(s, end); }
double WINAPI w_atof(const char* s) { return ::atof(s); }
int WINAPI w_atoi(const char* s) { return w_strtol(s, nullptr, 10); }
int64_t WINAPI w_atoll(const char* s) { return ::atoll(s); }

char* WINAPI w_itoa(int value, char* buf, int radix) {
    if (!buf) return nullptr;
    if (radix < 2 || radix > 36) {
        buf[0] = 0;
        errno = EINVAL;
        return buf;
    }
    char tmp[40];
    int n = 0;
    const bool neg = value < 0 && radix == 10;
    uint32_t u = neg ? 0u - static_cast<uint32_t>(value) : static_cast<uint32_t>(value);
    do {
        const uint32_t d = u % static_cast<uint32_t>(radix);
        tmp[n++] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10);
        u /= static_cast<uint32_t>(radix);
    } while (u);
    char* o = buf;
    if (neg) *o++ = '-';
    while (n) *o++ = tmp[--n];
    *o = 0;
    return buf;
}

thread_local uint32_t t_rand_seed = 1;
void WINAPI w_srand(uint32_t seed) { t_rand_seed = seed; }
int WINAPI w_rand() {
    t_rand_seed = t_rand_seed * 214013u + 2531011u;
    return static_cast<int>((t_rand_seed >> 16) & 0x7fff);
}

struct GuestCompare {
    gta6rt_gva fn;
};

int guest_compare(const GuestCompare& c, const void* a, const void* b) {
    return static_cast<int32_t>(gta6rt_isa_call_guest4(c.fn, reinterpret_cast<uintptr_t>(a), reinterpret_cast<uintptr_t>(b), 0, 0));
}

void WINAPI w_qsort(void* base, size_t count, size_t size, void* cmp) {
    if (!base || size == 0 || count < 2 || !cmp) return;
    GuestCompare c{reinterpret_cast<uintptr_t>(cmp)};
    ::qsort_r(base, count, size, &c, [](void* ctx, const void* a, const void* b) {
        return guest_compare(*static_cast<GuestCompare*>(ctx), a, b);
    });
}

void* WINAPI w_bsearch(const void* key, const void* base, size_t count, size_t size, void* cmp) {
    if (!key || !base || size == 0 || !cmp) return nullptr;
    GuestCompare c{reinterpret_cast<uintptr_t>(cmp)};
    size_t lo = 0, hi = count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const void* elem = static_cast<const char*>(base) + mid * size;
        const int r = guest_compare(c, key, elem);
        if (r == 0) return const_cast<void*>(elem);
        if (r < 0) hi = mid;
        else lo = mid + 1;
    }
    return nullptr;
}

void* WINAPI w_malloc(size_t n) { return std::malloc(n); }
void* WINAPI w_calloc(size_t n, size_t s) { return std::calloc(n, s); }
void* WINAPI w_realloc(void* p, size_t n) { return std::realloc(p, n); }
void WINAPI w_free(void* p) { std::free(p); }

int g_new_mode = 0;
int WINAPI w_set_new_mode(int mode) {
    const int old = g_new_mode;
    g_new_mode = mode;
    return old;
}

int WINAPI w_configthreadlocale(int mode) {
    (void)mode;
    return 0;
}

// Windows UCRT struct lconv, "C" locale.
struct WinLconv {
    char* decimal_point;
    char* thousands_sep;
    char* grouping;
    char* int_curr_symbol;
    char* currency_symbol;
    char* mon_decimal_point;
    char* mon_thousands_sep;
    char* mon_grouping;
    char* positive_sign;
    char* negative_sign;
    char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space, n_cs_precedes, n_sep_by_space, p_sign_posn,
        n_sign_posn;
    char16_t* w_decimal_point;
    char16_t* w_thousands_sep;
    char16_t* w_int_curr_symbol;
    char16_t* w_currency_symbol;
    char16_t* w_mon_decimal_point;
    char16_t* w_mon_thousands_sep;
    char16_t* w_positive_sign;
    char16_t* w_negative_sign;
};
static_assert(sizeof(WinLconv) == 10 * 8 + 8 + 8 * 8);

WinLconv* WINAPI w_localeconv() {
    static char dot[] = ".", empty[] = "";
    static char16_t wdot[] = u".", wempty[] = u"";
    static WinLconv lc = {dot, empty, empty, empty, empty, empty, empty, empty, empty, empty,
                          CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX,
                          wdot, wempty, wempty, wempty, wempty, wempty, wempty, wempty};
    return &lc;
}

}  // namespace
}  // namespace rt::crt

namespace {
using namespace rt::crt;
#define CR(n) SHIM("ucrtbase.dll", #n, w_##n)
}

SHIM_TABLE(crt_convert,
    SHIM("ucrtbase.dll", "_itoa", w_itoa), CR(strtod), CR(strtof), CR(strtol), CR(strtoll), CR(strtoul), CR(strtoull),
    CR(atof), CR(atoi), CR(atoll), SHIM("ucrtbase.dll", "_strtoi64", w_strtoll),
    SHIM("ucrtbase.dll", "_strtoui64", w_strtoull),
    CR(rand), CR(srand), CR(qsort), CR(bsearch),
    CR(malloc), CR(calloc), CR(realloc), CR(free), SHIM("ucrtbase.dll", "_set_new_mode", w_set_new_mode),
    SHIM("ucrtbase.dll", "_configthreadlocale", w_configthreadlocale), CR(localeconv),
);
