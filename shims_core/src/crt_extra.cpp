// UCRT imports of the packaged MS DLLs (msvcp140, libHttpClient, vcruntime140) and of the exe that had no row:
// the "C" locale queries msvcp140's _Locinfo makes at DllMain, its time-name tables, wide string and conversion
// helpers, low-level io, scanf, thread start, and advapi32's ETW entry points (no consumer on this host: accepted).
// Only the "C" locale exists here; a request for any other locale fails the way an unknown locale name does.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <mutex>
#include <string>

#include <gta6rt/os.h>

#include "file_system.h"
#include "printf_engine.h"
#include "process_shim.h"
#include "shims.h"
#include "util.h"

namespace rt::crt {
namespace {

// ---- "C" locale -------------------------------------------------------------------------------------------

constexpr uint16_t kUpper = 0x1, kLower = 0x2, kDigit = 0x4, kSpace = 0x8, kPunct = 0x10, kControl = 0x20, kBlank = 0x40,
                   kHex = 0x80, kAlpha = 0x100;

struct CType {
    uint16_t t[257];
    CType() {
        t[0] = 0;
        for (int c = 0; c < 256; ++c) {
            uint16_t v = 0;
            if (c < 0x20 || c == 0x7f) v |= kControl;
            if (c >= 9 && c <= 13) v |= kSpace;
            if (c == 9) v |= kBlank;
            if (c == ' ') v |= kSpace | kBlank;
            if (c >= '0' && c <= '9') v |= kDigit | kHex;
            if (c >= 'A' && c <= 'Z') v |= kUpper | kAlpha;
            if (c >= 'a' && c <= 'z') v |= kLower | kAlpha;
            if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) v |= kHex;
            if (c > 0x20 && c < 0x7f && !(v & (kDigit | kAlpha))) v |= kPunct;
            t[c + 1] = v;
        }
    }
};
const CType g_ctype;

std::recursive_mutex& locale_mutex() {
    static auto* m = new std::recursive_mutex;
    return *m;
}

void WINAPI w_lock_locales() { locale_mutex().lock(); }
void WINAPI w_unlock_locales() { locale_mutex().unlock(); }

char g_c_locale[] = "C";
char* WINAPI w_setlocale(int category, const char* name) {
    if (category < 0 || category > 5) {
        errno = EINVAL;
        return nullptr;
    }
    if (!name || !*name || !std::strcmp(name, "C")) return g_c_locale;
    RTLOG(WARN, "setlocale(%d, \"%s\"): only the C locale exists", category, name);
    return nullptr;
}

const uint16_t* WINAPI w_pctype_func() { return g_ctype.t + 1; }
uint32_t WINAPI w_lc_codepage_func() { return 0; }
uint32_t WINAPI w_lc_collate_cp_func() { return 0; }
char16_t* g_locale_names[6] = {};
char16_t** WINAPI w_lc_locale_name_func() { return g_locale_names; }
int WINAPI w_mb_cur_max_func() { return 1; }

int WINAPI w_isupper(int c) { return c >= -1 && c < 256 ? (g_ctype.t[c + 1] & kUpper) : 0; }
int WINAPI w_iswdigit(uint16_t c) { return c >= '0' && c <= '9' ? kDigit : 0; }
int WINAPI w_iswxdigit(uint16_t c) { return c < 0x80 && (g_ctype.t[c + 1] & kHex) ? kHex : 0; }
int WINAPI w_iswspace(uint16_t c) {
    if (c < 0x80) return g_ctype.t[c + 1] & kSpace;
    return c == 0x85 || c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 ||
                   c == 0x202f || c == 0x205f || c == 0x3000
               ? kSpace
               : 0;
}
int WINAPI w_iswalnum(uint16_t c) {
    if (c < 0x80) return g_ctype.t[c + 1] & (kAlpha | kDigit);
    return std::iswalnum(static_cast<wint_t>(c)) ? kAlpha : 0;
}
uint16_t WINAPI w_btowc(int c) { return c >= 0 && c < 256 ? static_cast<uint16_t>(c) : 0xffff; }

// ---- time names -------------------------------------------------------------------------------------------

const char* const kDayAbbr[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* const kDay[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const char* const kMonAbbr[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
const char* const kMon[12] = {"January", "February", "March",     "April",   "May",      "June",
                              "July",    "August",   "September", "October", "November", "December"};

std::string names(const char* const* abbr, const char* const* full, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += std::string(":") + abbr[i] + ":" + full[i];
    return s;
}

char* dup_narrow(const std::string& s) {
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (p) std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

char16_t* dup_wide(const std::string& s) {
    const std::u16string w = widen(s.c_str());
    auto* p = static_cast<char16_t*>(std::malloc((w.size() + 1) * sizeof(char16_t)));
    if (p) {
        std::memcpy(p, w.data(), w.size() * sizeof(char16_t));
        p[w.size()] = 0;
    }
    return p;
}

char* WINAPI w_Getdays() { return dup_narrow(names(kDayAbbr, kDay, 7)); }
char* WINAPI w_Getmonths() { return dup_narrow(names(kMonAbbr, kMon, 12)); }
char16_t* WINAPI w_W_Getdays() { return dup_wide(names(kDayAbbr, kDay, 7)); }
char16_t* WINAPI w_W_Getmonths() { return dup_wide(names(kMonAbbr, kMon, 12)); }

// __crt_lc_time_data (0x2c0 bytes). One malloc block, freed by the caller with free().
void* make_time_names() {
    struct Block {
        char* wday_abbr[7];
        char* wday[7];
        char* month_abbr[12];
        char* month[12];
        char* ampm[2];
        char* sdatefmt;
        char* ldatefmt;
        char* timefmt;
        int32_t caltype;
        int32_t refcount;
        char16_t* w_wday_abbr[7];
        char16_t* w_wday[7];
        char16_t* w_month_abbr[12];
        char16_t* w_month[12];
        char16_t* w_ampm[2];
        char16_t* w_sdatefmt;
        char16_t* w_ldatefmt;
        char16_t* w_timefmt;
        char16_t* w_locale_name;
    };
    static_assert(sizeof(Block) == 0x2c0, "__crt_lc_time_data layout");
    struct Wide {
        char16_t text[2048];
        size_t used = 0;
        char16_t* add(const char* s) {
            char16_t* p = text + used;
            while (*s) text[used++] = static_cast<char16_t>(*s++);
            text[used++] = 0;
            return p;
        }
    };
    struct All {
        Block b;
        Wide w;
    };
    auto* a = static_cast<All*>(std::calloc(1, sizeof(All)));
    if (!a) return nullptr;
    for (int i = 0; i < 7; ++i) {
        a->b.wday_abbr[i] = const_cast<char*>(kDayAbbr[i]);
        a->b.wday[i] = const_cast<char*>(kDay[i]);
        a->b.w_wday_abbr[i] = a->w.add(kDayAbbr[i]);
        a->b.w_wday[i] = a->w.add(kDay[i]);
    }
    for (int i = 0; i < 12; ++i) {
        a->b.month_abbr[i] = const_cast<char*>(kMonAbbr[i]);
        a->b.month[i] = const_cast<char*>(kMon[i]);
        a->b.w_month_abbr[i] = a->w.add(kMonAbbr[i]);
        a->b.w_month[i] = a->w.add(kMon[i]);
    }
    static char am[] = "AM", pm[] = "PM", sdate[] = "MM/dd/yy", ldate[] = "dddd, MMMM dd, yyyy", tfmt[] = "HH:mm:ss";
    a->b.ampm[0] = am;
    a->b.ampm[1] = pm;
    a->b.sdatefmt = sdate;
    a->b.ldatefmt = ldate;
    a->b.timefmt = tfmt;
    a->b.w_ampm[0] = a->w.add(am);
    a->b.w_ampm[1] = a->w.add(pm);
    a->b.w_sdatefmt = a->w.add(sdate);
    a->b.w_ldatefmt = a->w.add(ldate);
    a->b.w_timefmt = a->w.add(tfmt);
    a->b.w_locale_name = a->w.add("C");
    a->b.caltype = 1;
    a->b.refcount = 1;
    return a;
}

void* WINAPI w_Gettnames() { return make_time_names(); }
void* WINAPI w_W_Gettnames() { return make_time_names(); }

struct WinTm {
    int32_t tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
};

tm host_tm(const WinTm* w) {
    tm t;
    std::memset(&t, 0, sizeof t);
    t.tm_sec = w->tm_sec; t.tm_min = w->tm_min; t.tm_hour = w->tm_hour; t.tm_mday = w->tm_mday; t.tm_mon = w->tm_mon;
    t.tm_year = w->tm_year; t.tm_wday = w->tm_wday; t.tm_yday = w->tm_yday; t.tm_isdst = w->tm_isdst;
    return t;
}

// MSVC's '#' flag (no leading zeros / long form) has no host equivalent; it is dropped.
std::string host_time_format(const char* f) {
    std::string out;
    for (; *f; ++f) {
        out.push_back(*f);
        if (*f == '%' && f[1] == '#') ++f;
    }
    return out;
}

size_t WINAPI w_Strftime(char* s, size_t max, const char* fmt, const WinTm* t, void*) {
    if (!s || !fmt || !t || !max) {
        errno = EINVAL;
        return 0;
    }
    const tm ht = host_tm(t);
    return std::strftime(s, max, host_time_format(fmt).c_str(), &ht);
}

size_t WINAPI w_Wcsftime(char16_t* s, size_t max, const char16_t* fmt, const WinTm* t, void*) {
    if (!s || !fmt || !t || !max) {
        errno = EINVAL;
        return 0;
    }
    const tm ht = host_tm(t);
    std::string buf(max * 4 + 64, '\0');
    const size_t n = std::strftime(buf.data(), buf.size(), host_time_format(narrow(fmt).c_str()).c_str(), &ht);
    if (!n && !narrow(fmt).empty()) {
        s[0] = 0;
        return 0;
    }
    const std::u16string w = widen(buf.c_str(), n);
    if (w.size() + 1 > max) {
        s[0] = 0;
        return 0;
    }
    std::memcpy(s, w.data(), w.size() * sizeof(char16_t));
    s[w.size()] = 0;
    return w.size();
}

// ---- wide strings and conversions ------------------------------------------------------------------------

char16_t* WINAPI w_wcsdup(const char16_t* s) {
    if (!s) return nullptr;
    const size_t n = u16len(s);
    auto* p = static_cast<char16_t*>(std::malloc((n + 1) * sizeof(char16_t)));
    if (p) std::memcpy(p, s, (n + 1) * sizeof(char16_t));
    return p;
}

int WINAPI w_wcscpy_s(char16_t* dst, size_t size, const char16_t* src) {
    if (!dst || !size) return EINVAL;
    if (!src) {
        dst[0] = 0;
        return EINVAL;
    }
    const size_t n = u16len(src);
    if (n >= size) {
        dst[0] = 0;
        return ERANGE;
    }
    std::memcpy(dst, src, (n + 1) * sizeof(char16_t));
    return 0;
}

size_t WINAPI w_wcsnlen(const char16_t* s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) ++n;
    return n;
}

size_t WINAPI w_strncnt(const char* s, size_t n) {
    size_t i = 0;
    while (i < n && s[i]) ++i;
    return i;
}

bool in_set(char16_t c, const char16_t* set) {
    for (; *set; ++set)
        if (*set == c) return true;
    return false;
}

char16_t* WINAPI w_wcstok_s(char16_t* str, const char16_t* delim, char16_t** ctx) {
    if (!delim || !ctx || (!str && !*ctx)) {
        errno = EINVAL;
        return nullptr;
    }
    char16_t* p = str ? str : *ctx;
    while (*p && in_set(*p, delim)) ++p;
    if (!*p) {
        *ctx = p;
        return nullptr;
    }
    char16_t* tok = p;
    while (*p && !in_set(*p, delim)) ++p;
    if (*p) *p++ = 0;
    *ctx = p;
    return tok;
}

std::string ascii_prefix(const char16_t* s, size_t* units) {
    std::string out;
    size_t i = 0;
    for (; s && s[i] && s[i] < 0x80; ++i) out.push_back(static_cast<char>(s[i]));
    if (units) *units = i;
    return out;
}

int WINAPI w_wtoi(const char16_t* s) { return static_cast<int>(std::strtol(ascii_prefix(s, nullptr).c_str(), nullptr, 10)); }

uint64_t WINAPI w_wcstoull(const char16_t* s, char16_t** end, int base) {
    const std::string a = ascii_prefix(s, nullptr);
    char* e = nullptr;
    const unsigned long long v = std::strtoull(a.c_str(), &e, base);
    if (end) *end = const_cast<char16_t*>(s) + (e - a.c_str());
    return v;
}

size_t WINAPI w_wcstombs(char* dst, const char16_t* src, size_t n) {
    if (!src) {
        errno = EINVAL;
        return static_cast<size_t>(-1);
    }
    size_t i = 0;
    for (;; ++i) {
        const char16_t c = src[i];
        if (c > 0xff) {
            errno = EILSEQ;
            return static_cast<size_t>(-1);
        }
        if (!c) {
            if (dst && i < n) dst[i] = 0;
            return i;
        }
        if (dst) {
            if (i >= n) return i;
            dst[i] = static_cast<char>(c);
        }
    }
}

int32_t WINAPI w_atol(const char* s) { return static_cast<int32_t>(std::strtol(s, nullptr, 10)); }

double WINAPI w_frexp(double x, int* e) { return std::frexp(x, e); }
double WINAPI w_ldexp(double x, int e) { return std::ldexp(x, e); }

int WINAPI w_rand_s(uint32_t* out) {
    if (!out) return EINVAL;
    *out = arc4random();
    return 0;
}

int WINAPI w_putenv_s(const char* name, const char* value) {
    if (!name || !value || !*name || std::strchr(name, '=')) return EINVAL;
    const gta6rt_status st = gta6rt_env_set(name, *value ? value : nullptr);
    RTLOG(INFO, "crt: _putenv_s(\"%s\", \"%s\") -> %s", name, value, gta6rt_status_name(st));
    return st == GTA6RT_OK ? 0 : EINVAL;
}

char* WINAPI w_strerror(int e) {
    if (e == 11) return const_cast<char*>("Resource temporarily unavailable");
    return std::strerror(e);
}

// ---- runtime ---------------------------------------------------------------------------------------------

std::atomic<uint64_t> g_new_handler{0};

uint64_t WINAPI w_set_new_handler(uint64_t fn) { return g_new_handler.exchange(fn); }

int WINAPI w_callnewh(size_t size) {
    const uint64_t h = g_new_handler.load();
    if (!h) return 0;
    const uint64_t args[1] = {size};
    return static_cast<int>(gta6rt_os_invoke(h, args, 1));
}

// Through the registered kernel32 rows (os/), so this file needs no kernel32 symbol of its own.
using CreateThreadFn = uint64_t(WINAPI*)(void*, size_t, uint64_t, void*, uint32_t, uint32_t*);
using ExitThreadFn = void(WINAPI*)(uint32_t);

uint64_t WINAPI w_beginthreadex(void* security, uint32_t stack, uint64_t start, void* arg, uint32_t flags, uint32_t* tid) {
    static const auto create = reinterpret_cast<CreateThreadFn>(rt::shims::find("kernel32.dll", "CreateThread"));
    if (!start || !create) {
        errno = start ? EAGAIN : EINVAL;
        return 0;
    }
    const uint64_t h = create(security, stack, start, arg, flags, tid);
    if (!h) errno = EAGAIN;
    return h;
}

void WINAPI w_endthreadex(uint32_t code) {
    static const auto exit_thread = reinterpret_cast<ExitThreadFn>(rt::shims::find("kernel32.dll", "ExitThread"));
    if (exit_thread) exit_thread(code);
    RTLOG(ERROR, "_endthreadex: no ExitThread row; terminating");
    rt::process::abort_run(3);
}

int WINAPI w_seh_filter(uint32_t code, void*) {
    (void)code;
    return 0;
}

int WINAPI w_crt_at_quick_exit(uint64_t) { return 0; }

void WINAPI w_invoke_watson(const char16_t* expr, const char16_t* fn, const char16_t* file, uint32_t line, uint64_t) {
    RTLOG(ERROR, "_invoke_watson: invalid parameter (%s in %s at %s:%u); terminating", narrow(expr).c_str(),
          narrow(fn).c_str(), narrow(file).c_str(), line);
    rt::process::abort_run(static_cast<int>(0xC0000409u));
}

void WINAPI w_wassert(const char16_t* msg, const char16_t* file, uint32_t line) {
    RTLOG(ERROR, "assertion failed: %s (%s:%u); terminating", narrow(msg).c_str(), narrow(file).c_str(), line);
    rt::process::abort_run(3);
}

// ---- low-level io (fds are the host's) ------------------------------------------------------------------

int open_guest_fd(const std::string& path, int oflag, int pmode) {
    int flags = 0;
    switch (oflag & 3) {
    case 0: flags = O_RDONLY; break;
    case 1: flags = O_WRONLY; break;
    default: flags = O_RDWR; break;
    }
    if (oflag & 0x8) flags |= O_APPEND;
    if (oflag & 0x100) flags |= O_CREAT;
    if (oflag & 0x200) flags |= O_TRUNC;
    if (oflag & 0x400) flags |= O_EXCL;
    const bool writes = (oflag & 3) != 0 || (oflag & 0x300);
    const fs::HostPath hp = writes ? fs::crt_write_path(path, !(oflag & 0x200)) : fs::crt_read_path(path);
    if (hp.error) {
        errno = hp.error == 2 || hp.error == 3 ? ENOENT : EACCES;
        return -1;
    }
    return ::open(hp.path.c_str(), flags, (pmode & 0x80) ? 0644 : 0444);
}

int WINAPI w_open(const char* path, int oflag, int pmode) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }
    return open_guest_fd(path, oflag, pmode);
}
int WINAPI w_wopen(const char16_t* path, int oflag, int pmode) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }
    return open_guest_fd(narrow(path), oflag, pmode);
}
int WINAPI w_close(int fd) { return ::close(fd); }
int WINAPI w_read(int fd, void* buf, uint32_t n) { return static_cast<int>(::read(fd, buf, n)); }
int WINAPI w_write(int fd, const void* buf, uint32_t n) { return static_cast<int>(::write(fd, buf, n)); }
int64_t WINAPI w_lseeki64(int fd, int64_t off, int origin) { return ::lseek(fd, off, origin); }

// ---- wide file-system calls of msvcp140's <filesystem> -------------------------------------------------

int WINAPI w_wremove(const char16_t* path) {
    uint32_t error = 0;
    if (!path || !fs::crt_remove(narrow(path), error)) {
        errno = error == 2 || error == 3 ? ENOENT : EACCES;
        return -1;
    }
    return 0;
}

bool inside_package(const std::string& host) {
    const std::string game = fs::game_dir();
    return !game.empty() && host.size() > game.size() && host.compare(0, game.size(), game) == 0 && host[game.size()] == '/';
}

int WINAPI w_wrename(const char16_t* from, const char16_t* to) {
    if (!from || !to) {
        errno = EINVAL;
        return -1;
    }
    const fs::HostPath src = fs::crt_read_path(narrow(from));
    const fs::HostPath dst = fs::crt_write_path(narrow(to), false);
    if (src.error || dst.error) {
        errno = ENOENT;
        return -1;
    }
    if (!inside_package(src.path)) return ::rename(src.path.c_str(), dst.path.c_str());
    FILE* in = std::fopen(src.path.c_str(), "rb");
    FILE* out = in ? std::fopen(dst.path.c_str(), "wb") : nullptr;
    bool ok = in && out;
    char buf[65536];
    for (size_t n; ok && (n = std::fread(buf, 1, sizeof buf, in)) > 0;) ok = std::fwrite(buf, 1, n, out) == n;
    if (in) std::fclose(in);
    if (out) std::fclose(out);
    uint32_t error = 0;
    if (!ok || !fs::crt_remove(narrow(from), error)) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

int WINAPI w_wrmdir(const char16_t* path) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }
    const fs::HostPath hp = fs::crt_write_path(narrow(path), false);
    if (hp.error) {
        errno = ENOENT;
        return -1;
    }
    return ::rmdir(hp.path.c_str());
}

int WINAPI w_wchdir(const char16_t* path) {
    RTLOG(WARN, "_wchdir(%s): the working directory stays G:\\", narrow(path).c_str());
    errno = EACCES;
    return -1;
}

char16_t* WINAPI w_wgetcwd(char16_t* buf, int size) {
    static const char16_t kCwd[] = u"G:\\";
    const int need = 4;
    if (!buf) {
        auto* p = static_cast<char16_t*>(std::malloc((size > need ? size : need) * sizeof(char16_t)));
        if (p) std::memcpy(p, kCwd, sizeof kCwd);
        return p;
    }
    if (size < need) {
        errno = ERANGE;
        return nullptr;
    }
    std::memcpy(buf, kCwd, sizeof kCwd);
    return buf;
}

// ---- scanf over a Win64 va_list (every conversion argument is a pointer slot) ---------------------------

struct ScanSpec {
    bool wide_default;
    bool secure;
};

size_t utf16_units(const std::string& utf8, size_t bytes) { return widen(utf8.c_str(), bytes).size(); }

void store_int(void* p, int len, uint64_t v) {
    switch (len) {
    case 1: *static_cast<uint8_t*>(p) = static_cast<uint8_t>(v); break;
    case 2: *static_cast<uint16_t*>(p) = static_cast<uint16_t>(v); break;
    case 8: *static_cast<uint64_t*>(p) = v; break;
    default: *static_cast<uint32_t*>(p) = static_cast<uint32_t>(v); break;
    }
}

int scan_core(const std::string& in, const std::string& fmt, SlotReader ap, ScanSpec spec) {
    const char* s = in.c_str();
    const char* const s0 = s;
    const char* f = fmt.c_str();
    int assigned = 0;
    bool converted = false;
    auto input_failure = [&]() { return converted ? assigned : -1; };
    while (*f) {
        if (std::isspace(static_cast<unsigned char>(*f))) {
            while (std::isspace(static_cast<unsigned char>(*s))) ++s;
            while (std::isspace(static_cast<unsigned char>(*f))) ++f;
            continue;
        }
        if (*f != '%') {
            if (!*s) return input_failure();
            if (*s != *f) return assigned;
            ++s;
            ++f;
            continue;
        }
        ++f;
        if (*f == '%') {
            while (std::isspace(static_cast<unsigned char>(*s))) ++s;
            if (*s != '%') return *s ? assigned : input_failure();
            ++s;
            ++f;
            continue;
        }
        const bool suppress = *f == '*';
        if (suppress) ++f;
        int width = 0;
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        int len = 4;
        int wide = -1;
        bool dbl = false;
        if (f[0] == 'h' && f[1] == 'h') { len = 1; f += 2; }
        else if (f[0] == 'h') { len = 2; wide = 0; ++f; }
        else if (f[0] == 'l' && f[1] == 'l') { len = 8; f += 2; }
        else if (f[0] == 'l') { len = 4; wide = 1; dbl = true; ++f; }
        else if (f[0] == 'L') { len = 8; dbl = true; ++f; }
        else if (f[0] == 'I' && f[1] == '6' && f[2] == '4') { len = 8; f += 3; }
        else if (f[0] == 'I' && f[1] == '3' && f[2] == '2') { len = 4; f += 3; }
        else if (f[0] == 'I' || f[0] == 'z' || f[0] == 'j' || f[0] == 't') { len = 8; ++f; }
        else if (f[0] == 'w') { wide = 1; ++f; }
        char conv = *f++;
        if (!conv) return assigned;
        std::string set;
        bool negate = false;
        if (conv == '[') {
            if (*f == '^') { negate = true; ++f; }
            if (*f == ']') set.push_back(*f++);
            while (*f && *f != ']') set.push_back(*f++);
            if (*f == ']') ++f;
        }
        if (conv == 'n') {
            if (!suppress) store_int(reinterpret_cast<void*>(ap.next()), len, spec.wide_default ? utf16_units(in, s - s0) : s - s0);
            continue;
        }
        if (conv != 'c' && conv != '[')
            while (std::isspace(static_cast<unsigned char>(*s))) ++s;
        if (!*s) return input_failure();
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
            const char host_conv = conv == 'X' ? 'x' : conv == 'p' ? 'x' : conv;
            char hf[32];
            if (width) std::snprintf(hf, sizeof hf, "%%%dll%c%%n", width, host_conv);
            else std::snprintf(hf, sizeof hf, "%%ll%c%%n", host_conv);
            unsigned long long v = 0;
            int used = 0;
            if (std::sscanf(s, hf, &v, &used) != 1) return assigned;
            s += used;
            converted = true;
            if (!suppress) {
                store_int(reinterpret_cast<void*>(ap.next()), conv == 'p' ? 8 : len, v);
                ++assigned;
            }
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
            char hf[32];
            if (width) std::snprintf(hf, sizeof hf, "%%%dlf%%n", width);
            else std::snprintf(hf, sizeof hf, "%%lf%%n");
            double v = 0;
            int used = 0;
            if (std::sscanf(s, hf, &v, &used) != 1) return assigned;
            s += used;
            converted = true;
            if (!suppress) {
                void* p = reinterpret_cast<void*>(ap.next());
                if (dbl) *static_cast<double*>(p) = v;
                else *static_cast<float*>(p) = static_cast<float>(v);
                ++assigned;
            }
            break;
        }
        case 's': case 'S': case 'c': case 'C': case '[': {
            const bool upper = conv == 'S' || conv == 'C';
            bool out_wide = wide >= 0 ? wide == 1 : spec.wide_default;
            if (upper && wide < 0) out_wide = !spec.wide_default;
            const char lc = conv == '[' ? '[' : static_cast<char>(std::tolower(conv));
            const int limit = width ? width : (lc == 'c' ? 1 : 0x7fffffff);
            const char* start = s;
            int n = 0;
            if (lc == 'c') {
                while (*s && n < limit) { ++s; ++n; }
                if (n < limit) return input_failure();
            } else if (lc == 's') {
                while (*s && !std::isspace(static_cast<unsigned char>(*s)) && n < limit) { ++s; ++n; }
            } else {
                while (*s && n < limit && ((set.find(*s) != std::string::npos) != negate)) { ++s; ++n; }
                if (!n) return assigned;
            }
            converted = true;
            if (suppress) break;
            void* p = reinterpret_cast<void*>(ap.next());
            size_t cap = static_cast<size_t>(-1);
            if (spec.secure) cap = static_cast<uint32_t>(ap.next());
            const std::string text(start, s - start);
            const bool terminate = lc != 'c';
            if (out_wide) {
                const std::u16string w = widen(text.c_str(), text.size());
                if (w.size() + (terminate ? 1 : 0) > cap) {
                    if (cap) static_cast<char16_t*>(p)[0] = 0;
                    return assigned;
                }
                std::memcpy(p, w.data(), w.size() * sizeof(char16_t));
                if (terminate) static_cast<char16_t*>(p)[w.size()] = 0;
            } else {
                if (text.size() + (terminate ? 1 : 0) > cap) {
                    if (cap) static_cast<char*>(p)[0] = 0;
                    return assigned;
                }
                std::memcpy(p, text.data(), text.size());
                if (terminate) static_cast<char*>(p)[text.size()] = 0;
            }
            ++assigned;
            break;
        }
        default:
            RTLOG(WARN, "scanf: conversion '%c' is not supported", conv);
            return assigned;
        }
    }
    return assigned;
}

constexpr uint64_t kScanfSecure = 0x1;

int WINAPI w_vsscanf(uint64_t options, const char* buffer, size_t count, const char* fmt, void*, const uint64_t* args) {
    if (!buffer || !fmt) {
        errno = EINVAL;
        return -1;
    }
    const size_t n = count == static_cast<size_t>(-1) ? std::strlen(buffer) : strnlen(buffer, count);
    return scan_core(std::string(buffer, n), fmt, SlotReader{args}, ScanSpec{false, (options & kScanfSecure) != 0});
}

int WINAPI w_vswscanf(uint64_t options, const char16_t* buffer, size_t count, const char16_t* fmt, void*, const uint64_t* args) {
    if (!buffer || !fmt) {
        errno = EINVAL;
        return -1;
    }
    size_t n = 0;
    while ((count == static_cast<size_t>(-1) || n < count) && buffer[n]) ++n;
    return scan_core(narrow(buffer, n), narrow(fmt), SlotReader{args}, ScanSpec{true, (options & kScanfSecure) != 0});
}

// ---- advapi32 ETW: no session exists on this host --------------------------------------------------------

uint32_t WINAPI w_EventRegister(const void*, uint64_t, void*, uint64_t* handle) {
    static std::atomic<uint64_t> next{0x1000};
    if (handle) *handle = next.fetch_add(1);
    return 0;
}
uint32_t WINAPI w_EventUnregister(uint64_t) { return 0; }
uint32_t WINAPI w_EventSetInformation(uint64_t, int, void*, uint32_t) { return 0; }
uint32_t WINAPI w_EventWriteTransfer(uint64_t, const void*, const void*, const void*, uint32_t, void*) { return 0; }

}  // namespace
}  // namespace rt::crt

namespace {
using namespace rt::crt;
#define CX(n) SHIM("ucrtbase.dll", #n, w_##n)
}  // namespace

SHIM_TABLE(crt_extra,
    SHIM("ucrtbase.dll", "_lock_locales", w_lock_locales), SHIM("ucrtbase.dll", "_unlock_locales", w_unlock_locales),
    CX(setlocale), SHIM("ucrtbase.dll", "__pctype_func", w_pctype_func),
    SHIM("ucrtbase.dll", "___lc_codepage_func", w_lc_codepage_func),
    SHIM("ucrtbase.dll", "___lc_collate_cp_func", w_lc_collate_cp_func),
    SHIM("ucrtbase.dll", "___lc_locale_name_func", w_lc_locale_name_func),
    SHIM("ucrtbase.dll", "___mb_cur_max_func", w_mb_cur_max_func),
    CX(isupper), CX(iswdigit), CX(iswxdigit), CX(iswspace), CX(iswalnum), CX(btowc),
    SHIM("ucrtbase.dll", "_Getdays", w_Getdays), SHIM("ucrtbase.dll", "_Getmonths", w_Getmonths),
    SHIM("ucrtbase.dll", "_W_Getdays", w_W_Getdays), SHIM("ucrtbase.dll", "_W_Getmonths", w_W_Getmonths),
    SHIM("ucrtbase.dll", "_Gettnames", w_Gettnames), SHIM("ucrtbase.dll", "_W_Gettnames", w_W_Gettnames),
    SHIM("ucrtbase.dll", "_Strftime", w_Strftime), SHIM("ucrtbase.dll", "_Wcsftime", w_Wcsftime),
    SHIM("ucrtbase.dll", "_wcsdup", w_wcsdup), CX(wcscpy_s), CX(wcsnlen), SHIM("ucrtbase.dll", "__strncnt", w_strncnt),
    CX(wcstok_s), SHIM("ucrtbase.dll", "_wtoi", w_wtoi), CX(wcstoull), CX(wcstombs), CX(atol), CX(frexp), CX(ldexp),
    CX(rand_s), CX(strerror), SHIM("ucrtbase.dll", "_putenv_s", w_putenv_s),
    SHIM("ucrtbase.dll", "_set_new_handler", w_set_new_handler), SHIM("ucrtbase.dll", "_callnewh", w_callnewh),
    SHIM("ucrtbase.dll", "_beginthreadex", w_beginthreadex), SHIM("ucrtbase.dll", "_endthreadex", w_endthreadex),
    SHIM("ucrtbase.dll", "_seh_filter_dll", w_seh_filter), SHIM("ucrtbase.dll", "_seh_filter_exe", w_seh_filter),
    SHIM("ucrtbase.dll", "_crt_at_quick_exit", w_crt_at_quick_exit),
    SHIM("ucrtbase.dll", "_invoke_watson", w_invoke_watson), SHIM("ucrtbase.dll", "_wassert", w_wassert),
    SHIM("ucrtbase.dll", "_open", w_open), SHIM("ucrtbase.dll", "_wopen", w_wopen), SHIM("ucrtbase.dll", "_close", w_close),
    SHIM("ucrtbase.dll", "_read", w_read), SHIM("ucrtbase.dll", "_write", w_write),
    SHIM("ucrtbase.dll", "_lseeki64", w_lseeki64),
    SHIM("ucrtbase.dll", "_wremove", w_wremove), SHIM("ucrtbase.dll", "_wrename", w_wrename),
    SHIM("ucrtbase.dll", "_wrmdir", w_wrmdir), SHIM("ucrtbase.dll", "_wchdir", w_wchdir),
    SHIM("ucrtbase.dll", "_wgetcwd", w_wgetcwd),
    SHIM("ucrtbase.dll", "__stdio_common_vsscanf", w_vsscanf), SHIM("ucrtbase.dll", "__stdio_common_vswscanf", w_vswscanf),
    SHIM("advapi32.dll", "EventRegister", w_EventRegister), SHIM("advapi32.dll", "EventUnregister", w_EventUnregister),
    SHIM("advapi32.dll", "EventSetInformation", w_EventSetInformation),
    SHIM("advapi32.dll", "EventWriteTransfer", w_EventWriteTransfer),
);
