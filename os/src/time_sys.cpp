#include "internal.h"

#include <psapi.h>
#include <werapi.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <crt_externs.h>
#include <ctime>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <map>
#include <memory>
#include <strings.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <vector>

using namespace gta6rt_os;

namespace {

constexpr uint64_t kEpochDiff = 116444736000000000ull; // 1601 -> 1970 in 100 ns
constexpr uint64_t kTicksPerDay = 864000000000ull;

int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

void civil_from_days(int64_t z, int64_t* y, unsigned* m, unsigned* d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yy + (*m <= 2);
}

bool ft_to_st(uint64_t ft, SYSTEMTIME* st) {
    if (ft >= 0x8000000000000000ull) return false;
    uint64_t days = ft / kTicksPerDay;
    uint64_t rem = ft % kTicksPerDay;
    int64_t y;
    unsigned m, d;
    civil_from_days((int64_t)days - 134774, &y, &m, &d);
    st->wYear = (WORD)y;
    st->wMonth = (WORD)m;
    st->wDay = (WORD)d;
    st->wDayOfWeek = (WORD)((days + 1) % 7);
    st->wHour = (WORD)(rem / 36000000000ull);
    st->wMinute = (WORD)(rem / 600000000ull % 60);
    st->wSecond = (WORD)(rem / 10000000ull % 60);
    st->wMilliseconds = (WORD)(rem / 10000ull % 1000);
    return true;
}

bool st_to_ft(const SYSTEMTIME* st, uint64_t* ft) {
    static const unsigned mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (st->wYear < 1601 || st->wYear > 30827 || st->wMonth < 1 || st->wMonth > 12 || st->wHour > 23 ||
        st->wMinute > 59 || st->wSecond > 59 || st->wMilliseconds > 999 || st->wDay < 1)
        return false;
    bool leap = (st->wYear % 4 == 0 && st->wYear % 100 != 0) || st->wYear % 400 == 0;
    unsigned md = mdays[st->wMonth - 1] + (st->wMonth == 2 && leap ? 1 : 0);
    if (st->wDay > md) return false;
    int64_t days = days_from_civil(st->wYear, st->wMonth, st->wDay) + 134774;
    *ft = (uint64_t)days * kTicksPerDay + (uint64_t)st->wHour * 36000000000ull + (uint64_t)st->wMinute * 600000000ull +
          (uint64_t)st->wSecond * 10000000ull + (uint64_t)st->wMilliseconds * 10000ull;
    return true;
}

int64_t local_bias_100ns() {
    time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    return (int64_t)lt.tm_gmtoff * 10000000ll;
}

uint64_t get_ft(const FILETIME* f) { return ((uint64_t)f->dwHighDateTime << 32) | f->dwLowDateTime; }
void put_ft(FILETIME* f, uint64_t v) {
    f->dwLowDateTime = (DWORD)v;
    f->dwHighDateTime = (DWORD)(v >> 32);
}

uint64_t sysctl_u64(const char* name, uint64_t def) {
    uint64_t v = 0;
    size_t len = sizeof v;
    if (sysctlbyname(name, &v, &len, nullptr, 0) != 0) return def;
    if (len == 4) v &= 0xFFFFFFFFull;
    return v;
}

// ---- environment ----

struct EnvEntry {
    std::string name;
    std::unique_ptr<std::string> value;
};

pthread_mutex_t g_env_mx = PTHREAD_MUTEX_INITIALIZER;
std::map<std::string, EnvEntry>* g_env;

std::string env_key(const char* n) {
    std::string k(n);
    for (char& c : k) c = (char)toupper((unsigned char)c);
    return k;
}

void env_init_locked() {
    if (g_env) return;
    g_env = new std::map<std::string, EnvEntry>();
    for (char** e = *_NSGetEnviron(); e && *e; e++) {
        const char* eq = strchr(*e, '=');
        if (!eq || eq == *e) continue;
        std::string n(*e, eq - *e);
        (*g_env)[env_key(n.c_str())] = EnvEntry{n, std::make_unique<std::string>(eq + 1)};
    }
}

// ---- command line ----

pthread_mutex_t g_cmd_mx = PTHREAD_MUTEX_INITIALIZER;
std::string* g_cmd_a;
std::u16string* g_cmd_w;

void cmd_init_locked() {
    if (g_cmd_a) return;
    std::string s;
    int argc = *_NSGetArgc();
    char** argv = *_NSGetArgv();
    for (int i = 0; i < argc; i++) {
        if (i) s += ' ';
        bool q = strpbrk(argv[i], " \t\"") != nullptr || !*argv[i];
        if (q) s += '"';
        s += argv[i];
        if (q) s += '"';
    }
    g_cmd_a = new std::string(s);
    g_cmd_w = new std::u16string(utf8_to_u16(s.c_str()));
}

// ---- modules ----

struct ModEntry {
    gta6rt_module_desc d;
    std::string name, path, key;
    std::atomic<int> loads{1};
};

pthread_mutex_t g_mod_mx = PTHREAD_MUTEX_INITIALIZER;
std::vector<ModEntry*>* g_mods;
ModEntry* g_main;
gta6rt_module_load_fn g_load_hook;
void* g_load_user;

std::string mod_key(const char* n) {
    std::string s(n ? n : "");
    size_t p = s.find_last_of("/\\");
    if (p != std::string::npos) s = s.substr(p + 1);
    for (char& c : s) c = (char)tolower((unsigned char)c);
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".dll") == 0) s.resize(s.size() - 4);
    while (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

ModEntry* main_module_locked() {
    if (g_main) return g_main;
    if (g_mods)
        for (ModEntry* m : *g_mods)
            if (m->d.kind == GTA6RT_MODULE_GUEST_IMAGE) return m;
    return nullptr;
}

ModEntry* find_handle_locked(HMODULE h) {
    if (!h) return main_module_locked();
    if (g_mods)
        for (ModEntry* m : *g_mods)
            if (m->d.base == (gta6rt_gva)(uintptr_t)h) return m;
    return nullptr;
}

// ---- strings ----

const char16_t kCp1252[32] = {0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                              0x2039, 0x0152, 0x8D,   0x017D, 0x8F,   0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                              0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178};

bool utf8_cp(UINT cp) { return cp == CP_UTF8 || cp == CP_ACP || cp == CP_OEMCP || cp == CP_THREAD_ACP || cp == 20127; }

char16_t fold16(char16_t c) {
    if (c < 0x80) return (char16_t)toupper(c);
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return (char16_t)(c - 0x20);
    if (c >= 0x430 && c <= 0x44F) return (char16_t)(c - 0x20);
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return (char16_t)(c - 0x20);
    return c;
}

char16_t lower16(char16_t c) {
    if (c < 0x80) return (char16_t)tolower(c);
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return (char16_t)(c + 0x20);
    if (c >= 0x410 && c <= 0x42F) return (char16_t)(c + 0x20);
    if (c >= 0x391 && c <= 0x3A9) return (char16_t)(c + 0x20);
    return c;
}

int cmp16(const char16_t* a, int na, const char16_t* b, int nb, bool ic) {
    if (na < 0) na = (int)u16_len(a);
    if (nb < 0) nb = (int)u16_len(b);
    int n = na < nb ? na : nb;
    for (int i = 0; i < n; i++) {
        char16_t x = ic ? fold16(a[i]) : a[i], y = ic ? fold16(b[i]) : b[i];
        if (x != y) return x < y ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
    }
    return na == nb ? CSTR_EQUAL : na < nb ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
}

const char* kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const char* kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                         "July",    "August",   "September", "October", "November", "December"};

std::string format_picture(const SYSTEMTIME& st, const std::string& pic, bool date) {
    std::string out;
    size_t i = 0;
    char buf[32];
    while (i < pic.size()) {
        char c = pic[i];
        if (c == '\'') {
            size_t j = pic.find('\'', i + 1);
            if (j == std::string::npos) j = pic.size();
            out += pic.substr(i + 1, j - i - 1);
            i = j + 1;
            continue;
        }
        size_t n = 1;
        while (i + n < pic.size() && pic[i + n] == c) n++;
        bool handled = true;
        if (date && c == 'd') {
            if (n == 1) snprintf(buf, sizeof buf, "%u", st.wDay);
            else if (n == 2) snprintf(buf, sizeof buf, "%02u", st.wDay);
            else if (n == 3) snprintf(buf, sizeof buf, "%.3s", kDays[st.wDayOfWeek % 7]);
            else snprintf(buf, sizeof buf, "%s", kDays[st.wDayOfWeek % 7]);
        } else if (date && c == 'M') {
            if (n == 1) snprintf(buf, sizeof buf, "%u", st.wMonth);
            else if (n == 2) snprintf(buf, sizeof buf, "%02u", st.wMonth);
            else if (n == 3) snprintf(buf, sizeof buf, "%.3s", kMonths[(st.wMonth + 11) % 12]);
            else snprintf(buf, sizeof buf, "%s", kMonths[(st.wMonth + 11) % 12]);
        } else if (date && c == 'y') {
            if (n <= 2) snprintf(buf, sizeof buf, n == 1 ? "%u" : "%02u", st.wYear % 100);
            else snprintf(buf, sizeof buf, "%04u", st.wYear);
        } else if (!date && (c == 'h' || c == 'H')) {
            unsigned h = c == 'h' ? (st.wHour % 12 ? st.wHour % 12 : 12) : st.wHour;
            snprintf(buf, sizeof buf, n == 1 ? "%u" : "%02u", h);
        } else if (!date && c == 'm') {
            snprintf(buf, sizeof buf, n == 1 ? "%u" : "%02u", st.wMinute);
        } else if (!date && c == 's') {
            snprintf(buf, sizeof buf, n == 1 ? "%u" : "%02u", st.wSecond);
        } else if (!date && c == 't') {
            snprintf(buf, sizeof buf, "%s", n == 1 ? (st.wHour < 12 ? "A" : "P") : (st.wHour < 12 ? "AM" : "PM"));
        } else {
            handled = false;
        }
        if (handled) out += buf;
        else out.append(n, c);
        i += n;
    }
    return out;
}

int out_w(const std::string& s, LPWSTR dst, int cch) {
    std::u16string w = utf8_to_u16(s.c_str());
    if (cch == 0) return (int)w.size() + 1;
    if (!dst || cch < (int)w.size() + 1) {
        set_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    memcpy(dst, w.data(), w.size() * 2);
    dst[w.size()] = 0;
    return (int)w.size() + 1;
}

const char* error_text(DWORD code) {
    switch (code) {
    case ERROR_SUCCESS: return "The operation completed successfully.";
    case ERROR_FILE_NOT_FOUND: return "The system cannot find the file specified.";
    case ERROR_PATH_NOT_FOUND: return "The system cannot find the path specified.";
    case ERROR_ACCESS_DENIED: return "Access is denied.";
    case ERROR_INVALID_HANDLE: return "The handle is invalid.";
    case ERROR_NOT_ENOUGH_MEMORY: return "Not enough memory resources are available to process this command.";
    case ERROR_INVALID_PARAMETER: return "The parameter is incorrect.";
    case ERROR_INSUFFICIENT_BUFFER: return "The data area passed to a system call is too small.";
    case ERROR_ALREADY_EXISTS: return "Cannot create a file when that file already exists.";
    case ERROR_NOT_SUPPORTED: return "The request is not supported.";
    case ERROR_TIMEOUT: return "This operation returned because the timeout period expired.";
    default: return nullptr;
    }
}

} // namespace

namespace gta6rt_os {

uint32_t num_cpus() {
    static uint32_t n = [] {
        uint32_t c = (uint32_t)sysctl_u64("hw.activecpu", 8);
        if (const char* e = getenv("GTA6RT_CPU_COUNT")) c = (uint32_t)strtoul(e, nullptr, 0);
        if (c < 1) c = 1;
        if (c > 64) c = 64;
        return c;
    }();
    return n;
}

} // namespace gta6rt_os

extern "C" {

// ---- time ----

uint64_t gta6rt_time_monotonic_ns(void) { return monotonic_ns(); }
uint64_t gta6rt_time_qpc(void) { return monotonic_ns() / 100; }
uint64_t gta6rt_time_qpc_frequency(void) { return 10000000ull; }
uint64_t gta6rt_time_filetime_utc(void) { return filetime_now(); }

gta6rt_status gta6rt_random_bytes(void* buf, size_t size) {
    if (!buf && size) return GTA6RT_E_INVALID_ARG;
    arc4random_buf(buf, size);
    return GTA6RT_OK;
}

BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER* c) {
    c->QuadPart = (LONGLONG)gta6rt_time_qpc();
    return TRUE;
}

BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER* f) {
    f->QuadPart = 10000000;
    return TRUE;
}

ULONGLONG WINAPI GetTickCount64(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1000000ull; }
DWORD WINAPI GetTickCount(void) { return (DWORD)GetTickCount64(); }

BOOL WINAPI QueryUnbiasedInterruptTime(PULONGLONG t) {
    *t = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 100;
    return TRUE;
}

void WINAPI GetSystemTimeAsFileTime(LPFILETIME f) { put_ft(f, filetime_now()); }
void WINAPI GetSystemTimePreciseAsFileTime(LPFILETIME f) { put_ft(f, filetime_now()); }

void WINAPI GetSystemTime(LPSYSTEMTIME st) { ft_to_st(filetime_now(), st); }
void WINAPI GetLocalTime(LPSYSTEMTIME st) { ft_to_st(filetime_now() + local_bias_100ns(), st); }

BOOL WINAPI SystemTimeToFileTime(const SYSTEMTIME* st, LPFILETIME ft) {
    uint64_t v;
    if (!st_to_ft(st, &v)) {
        set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    put_ft(ft, v);
    return TRUE;
}

BOOL WINAPI FileTimeToSystemTime(const FILETIME* ft, LPSYSTEMTIME st) {
    if (!ft_to_st(get_ft(ft), st)) {
        set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI FileTimeToLocalFileTime(const FILETIME* ft, LPFILETIME lft) {
    put_ft(lft, get_ft(ft) + (uint64_t)local_bias_100ns());
    return TRUE;
}

BOOL WINAPI LocalFileTimeToFileTime(const FILETIME* lft, LPFILETIME ft) {
    put_ft(ft, get_ft(lft) - (uint64_t)local_bias_100ns());
    return TRUE;
}

LONG WINAPI CompareFileTime(const FILETIME* a, const FILETIME* b) {
    uint64_t x = get_ft(a), y = get_ft(b);
    return x < y ? -1 : x > y ? 1 : 0;
}

// ---- system information ----

void WINAPI GetSystemInfo(LPSYSTEM_INFO si) {
    memset(si, 0, sizeof *si);
    si->wProcessorArchitecture = PROCESSOR_ARCHITECTURE_AMD64;
    si->dwPageSize = GTA6RT_GUEST_PAGE_SIZE;
    si->lpMinimumApplicationAddress = (LPVOID)0x10000;
    si->lpMaximumApplicationAddress = (LPVOID)0x7FFFFFFEFFFFull;
    uint32_t n = num_cpus();
    si->dwActiveProcessorMask = n >= 64 ? ~0ull : ((1ull << n) - 1);
    si->dwNumberOfProcessors = n;
    si->dwProcessorType = PROCESSOR_AMD_X8664;
    si->dwAllocationGranularity = GTA6RT_GUEST_ALLOCATION_GRANULARITY;
    si->wProcessorLevel = 0x17;
    si->wProcessorRevision = 0x7100;
}

void WINAPI GetNativeSystemInfo(LPSYSTEM_INFO si) { GetSystemInfo(si); }

BOOL WINAPI GlobalMemoryStatusEx(LPMEMORYSTATUSEX m) {
    uint64_t total = sysctl_u64("hw.memsize", 16ull << 30);
    vm_statistics64_data_t vs;
    mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
    uint64_t avail = total / 2;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vs, &cnt) == KERN_SUCCESS)
        avail = ((uint64_t)vs.free_count + vs.inactive_count + vs.purgeable_count) * vm_kernel_page_size;
    if (avail > total) avail = total;
    m->dwLength = sizeof *m;
    m->dwMemoryLoad = (DWORD)(100 - avail * 100 / total);
    m->ullTotalPhys = total;
    m->ullAvailPhys = avail;
    m->ullTotalPageFile = total * 2;
    m->ullAvailPageFile = avail + total;
    m->ullTotalVirtual = 0x7FFFFFFE0000ull;
    m->ullAvailVirtual = 0x7FFFFFFE0000ull - (1ull << 34);
    m->ullAvailExtendedVirtual = 0;
    return TRUE;
}

BOOL WINAPI GetLogicalProcessorInformation(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buf, PDWORD len) {
    uint32_t n = num_cpus();
    uint64_t all = n >= 64 ? ~0ull : ((1ull << n) - 1);
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> v;
    auto add = [&](uint64_t mask, LOGICAL_PROCESSOR_RELATIONSHIP rel) -> SYSTEM_LOGICAL_PROCESSOR_INFORMATION& {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION e;
        memset(&e, 0, sizeof e);
        e.ProcessorMask = mask;
        e.Relationship = rel;
        v.push_back(e);
        return v.back();
    };
    uint32_t l1d = (uint32_t)sysctl_u64("hw.l1dcachesize", 32768), l1i = (uint32_t)sysctl_u64("hw.l1icachesize", 32768);
    uint32_t l2 = (uint32_t)sysctl_u64("hw.l2cachesize", 4u << 20);
    for (uint32_t i = 0; i < n; i++) {
        add(1ull << i, RelationProcessorCore).ProcessorCore.Flags = 0;
        auto& d = add(1ull << i, RelationCache);
        d.Cache = CACHE_DESCRIPTOR{1, 8, 64, l1d, CacheData};
        auto& c = add(1ull << i, RelationCache);
        c.Cache = CACHE_DESCRIPTOR{1, 8, 64, l1i, CacheInstruction};
    }
    add(all, RelationCache).Cache = CACHE_DESCRIPTOR{2, 16, 64, l2, CacheUnified};
    add(all, RelationProcessorPackage);
    add(all, RelationNumaNode).NumaNode.NodeNumber = 0;
    DWORD need = (DWORD)(v.size() * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (!len) {
        set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!buf || *len < need) {
        *len = need;
        set_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(buf, v.data(), need);
    *len = need;
    return TRUE;
}

DWORD WINAPI GetActiveProcessorCount(WORD) { return num_cpus(); }
WORD WINAPI GetActiveProcessorGroupCount(void) { return 1; }

BOOL WINAPI GetVersionExW(LPOSVERSIONINFOEXW vi) {
    if (!vi || vi->dwOSVersionInfoSize < 276) {
        set_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    DWORD sz = vi->dwOSVersionInfoSize;
    memset(((char*)vi) + 4, 0, (sz >= sizeof *vi ? sizeof *vi : 276) - 4);
    vi->dwMajorVersion = 10;
    vi->dwMinorVersion = 0;
    vi->dwBuildNumber = 19041;
    vi->dwPlatformId = 2;
    if (sz >= sizeof *vi) vi->wProductType = 1;
    return TRUE;
}

UINT WINAPI GetSystemDirectoryW(LPWSTR b, UINT n) { return copy_out_w(u"C:\\Windows\\System32", b, n); }
UINT WINAPI GetWindowsDirectoryW(LPWSTR b, UINT n) { return copy_out_w(u"C:\\Windows", b, n); }

BOOL WINAPI IsProcessorFeaturePresent(DWORD f) {
    switch (f) {
    case 2: case 3: case 6: case 8: case 9: case 10: case 12: case 13: case 14: case 17: case 20: case 23: case 32:
    case 36: case 37: case 38: case PF_AVX_INSTRUCTIONS_AVAILABLE: case PF_AVX2_INSTRUCTIONS_AVAILABLE: return TRUE;
    default: return FALSE;
    }
}

// ---- environment ----

const char* gta6rt_env_get(const char* name) {
    if (!name) return nullptr;
    pthread_mutex_lock(&g_env_mx);
    env_init_locked();
    auto it = g_env->find(env_key(name));
    const char* r = it == g_env->end() ? nullptr : it->second.value->c_str();
    pthread_mutex_unlock(&g_env_mx);
    return r;
}

gta6rt_status gta6rt_env_set(const char* name, const char* value_or_null) {
    if (!name || !*name || strchr(name, '=')) return GTA6RT_E_INVALID_ARG;
    pthread_mutex_lock(&g_env_mx);
    env_init_locked();
    std::string k = env_key(name);
    if (value_or_null) {
        auto it = g_env->find(k);
        if (it == g_env->end()) (*g_env)[k] = EnvEntry{name, std::make_unique<std::string>(value_or_null)};
        else it->second.value = std::make_unique<std::string>(value_or_null);
        setenv(name, value_or_null, 1);
    } else {
        auto it = g_env->find(k);
        if (it != g_env->end()) {
            unsetenv(it->second.name.c_str());
            g_env->erase(it);
        }
    }
    pthread_mutex_unlock(&g_env_mx);
    return GTA6RT_OK;
}

DWORD WINAPI GetEnvironmentVariableA(LPCSTR lpName, LPSTR lpBuffer, DWORD nSize) {
    pthread_mutex_lock(&g_env_mx);
    env_init_locked();
    auto it = lpName ? g_env->find(env_key(lpName)) : g_env->end();
    if (it == g_env->end()) {
        pthread_mutex_unlock(&g_env_mx);
        set_error(ERROR_ENVVAR_NOT_FOUND);
        return 0;
    }
    std::string v = *it->second.value;
    pthread_mutex_unlock(&g_env_mx);
    return copy_out_a(v, lpBuffer, nSize);
}

DWORD WINAPI GetEnvironmentVariableW(LPCWSTR lpName, LPWSTR lpBuffer, DWORD nSize) {
    std::string n = u16_to_utf8(lpName);
    pthread_mutex_lock(&g_env_mx);
    env_init_locked();
    auto it = lpName ? g_env->find(env_key(n.c_str())) : g_env->end();
    if (it == g_env->end()) {
        pthread_mutex_unlock(&g_env_mx);
        set_error(ERROR_ENVVAR_NOT_FOUND);
        return 0;
    }
    std::u16string v = utf8_to_u16(it->second.value->c_str());
    pthread_mutex_unlock(&g_env_mx);
    return copy_out_w(v, lpBuffer, nSize);
}

BOOL WINAPI SetEnvironmentVariableA(LPCSTR lpName, LPCSTR lpValue) {
    if (gta6rt_env_set(lpName, lpValue) != GTA6RT_OK) {
        set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI SetEnvironmentVariableW(LPCWSTR lpName, LPCWSTR lpValue) {
    std::string n = u16_to_utf8(lpName), v = u16_to_utf8(lpValue);
    return SetEnvironmentVariableA(lpName ? n.c_str() : nullptr, lpValue ? v.c_str() : nullptr);
}

DWORD WINAPI ExpandEnvironmentStringsW(LPCWSTR lpSrc, LPWSTR lpDst, DWORD nSize) {
    std::string s = u16_to_utf8(lpSrc), out;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '%') {
            size_t j = s.find('%', i + 1);
            if (j != std::string::npos && j > i + 1) {
                const char* v = gta6rt_env_get(s.substr(i + 1, j - i - 1).c_str());
                if (v) {
                    out += v;
                    i = j + 1;
                    continue;
                }
            }
        }
        out += s[i++];
    }
    std::u16string w = utf8_to_u16(out.c_str());
    if (lpDst && nSize >= w.size() + 1) {
        memcpy(lpDst, w.data(), w.size() * 2);
        lpDst[w.size()] = 0;
    }
    return (DWORD)w.size() + 1;
}

LPWCH WINAPI GetEnvironmentStringsW(void) {
    pthread_mutex_lock(&g_env_mx);
    env_init_locked();
    std::u16string block;
    for (auto& kv : *g_env) {
        block += utf8_to_u16((kv.second.name + "=" + *kv.second.value).c_str());
        block += u'\0';
    }
    pthread_mutex_unlock(&g_env_mx);
    block += u'\0';
    LPWCH r = (LPWCH)malloc(block.size() * 2);
    memcpy(r, block.data(), block.size() * 2);
    return r;
}

BOOL WINAPI FreeEnvironmentStringsW(LPWCH penv) {
    free(penv);
    return TRUE;
}

gta6rt_status gta6rt_process_set_command_line(const char* utf8) {
    if (!utf8) return GTA6RT_E_INVALID_ARG;
    pthread_mutex_lock(&g_cmd_mx);
    delete g_cmd_a;
    delete g_cmd_w;
    g_cmd_a = new std::string(utf8);
    g_cmd_w = new std::u16string(utf8_to_u16(utf8));
    pthread_mutex_unlock(&g_cmd_mx);
    return GTA6RT_OK;
}

LPSTR WINAPI GetCommandLineA(void) {
    pthread_mutex_lock(&g_cmd_mx);
    cmd_init_locked();
    LPSTR r = &(*g_cmd_a)[0];
    pthread_mutex_unlock(&g_cmd_mx);
    return r;
}

LPWSTR WINAPI GetCommandLineW(void) {
    pthread_mutex_lock(&g_cmd_mx);
    cmd_init_locked();
    LPWSTR r = &(*g_cmd_w)[0];
    pthread_mutex_unlock(&g_cmd_mx);
    return r;
}

// ---- strings and locale ----

int WINAPI MultiByteToWideChar(UINT cp, DWORD flags, LPCCH src, int n, LPWSTR dst, int cch) {
    if (!src || n == 0 || cch < 0 || (dst && (const void*)dst == (const void*)src)) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    size_t len = n < 0 ? strlen(src) + 1 : (size_t)n;
    std::u16string w;
    if (utf8_cp(cp)) {
        w = utf8_to_u16(src, len);
        if ((flags & MB_ERR_INVALID_CHARS) && w.find(u'\uFFFD') != std::u16string::npos &&
            u16_to_utf8(w.c_str(), w.size()) != std::string(src, len)) {
            set_error(ERROR_NO_UNICODE_TRANSLATION);
            return 0;
        }
    } else if (cp == 1252 || cp == 28591) {
        for (size_t i = 0; i < len; i++) {
            unsigned char c = (unsigned char)src[i];
            w += cp == 1252 && c >= 0x80 && c < 0xA0 ? kCp1252[c - 0x80] : (char16_t)c;
        }
    } else {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (cch == 0) return (int)w.size();
    if ((int)w.size() > cch) {
        set_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    memcpy(dst, w.data(), w.size() * 2);
    return (int)w.size();
}

int WINAPI WideCharToMultiByte(UINT cp, DWORD flags, LPCWCH src, int n, LPSTR dst, int cb, LPCCH defchar,
                               LPBOOL used_default) {
    if (!src || n == 0 || cb < 0) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    size_t len = n < 0 ? u16_len(src) + 1 : (size_t)n;
    std::string s;
    if (used_default) *used_default = FALSE;
    if (utf8_cp(cp)) {
        if (flags & WC_ERR_INVALID_CHARS)
            for (size_t i = 0; i < len; i++) {
                char16_t c = src[i];
                bool hi = c >= 0xD800 && c <= 0xDBFF, lo = c >= 0xDC00 && c <= 0xDFFF;
                if (lo || (hi && (i + 1 >= len || src[i + 1] < 0xDC00 || src[i + 1] > 0xDFFF))) {
                    set_error(ERROR_NO_UNICODE_TRANSLATION);
                    return 0;
                }
                if (hi) i++;
            }
        s = u16_to_utf8(src, len);
    } else if (cp == 1252 || cp == 28591) {
        char dc = defchar ? *defchar : '?';
        for (size_t i = 0; i < len; i++) {
            char16_t c = src[i];
            int b = -1;
            if (c < 0x80 || (c >= 0xA0 && c < 0x100) || (cp == 28591 && c < 0x100)) b = c;
            else if (cp == 1252)
                for (int k = 0; k < 32; k++)
                    if (kCp1252[k] == c) b = 0x80 + k;
            if (b < 0) {
                b = (unsigned char)dc;
                if (used_default) *used_default = TRUE;
            }
            s += (char)b;
        }
    } else {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (cb == 0) return (int)s.size();
    if ((int)s.size() > cb) {
        set_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    memcpy(dst, s.data(), s.size());
    return (int)s.size();
}

int WINAPI CompareStringOrdinal(LPCWCH a, int na, LPCWCH b, int nb, BOOL bIgnoreCase) {
    if (!a || !b) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return cmp16(a, na, b, nb, bIgnoreCase != 0);
}

int WINAPI CompareStringEx(LPCWSTR, DWORD flags, LPCWCH a, int na, LPCWCH b, int nb, PVOID, LPVOID, LPARAM) {
    if (!a || !b) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return cmp16(a, na, b, nb, (flags & (NORM_IGNORECASE | LINGUISTIC_IGNORECASE)) != 0);
}

int WINAPI GetUserDefaultLocaleName(LPWSTR name, int cch) { return out_w("en-US", name, cch); }
UINT WINAPI GetACP(void) { return CP_UTF8; }
UINT WINAPI GetOEMCP(void) { return CP_UTF8; }

int WINAPI lstrlenW(LPCWSTR s) { return s ? (int)u16_len(s) : 0; }
int WINAPI lstrcmpiW(LPCWSTR a, LPCWSTR b) { return cmp16(a ? a : u"", -1, b ? b : u"", -1, true) - 2; }
int WINAPI lstrcmpW(LPCWSTR a, LPCWSTR b) { return cmp16(a ? a : u"", -1, b ? b : u"", -1, false) - 2; }

LCID WINAPI LocaleNameToLCID(LPCWSTR lpName, DWORD) {
    if (!lpName || !*lpName) return 0x0409;
    std::string n = u16_to_utf8(lpName);
    if (!strcasecmp(n.c_str(), "en-US")) return 0x0409;
    if (!strcasecmp(n.c_str(), "en-GB")) return 0x0809;
    if (!strcasecmp(n.c_str(), "fr-FR")) return 0x040C;
    if (!strcasecmp(n.c_str(), "de-DE")) return 0x0407;
    if (!strcasecmp(n.c_str(), "es-ES")) return 0x0C0A;
    if (!strcasecmp(n.c_str(), "it-IT")) return 0x0410;
    if (!strcasecmp(n.c_str(), "ja-JP")) return 0x0411;
    return 0x1000; // LOCALE_CUSTOM_UNSPECIFIED
}

int WINAPI GetUserDefaultGeoName(LPWSTR geoName, int cch) { return out_w("US", geoName, cch); }

int WINAPI LCMapStringEx(LPCWSTR, DWORD flags, LPCWSTR src, int n, LPWSTR dst, int cch, LPNLSVERSIONINFO, LPVOID,
                         LPARAM) {
    if (!src) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    size_t len = n < 0 ? u16_len(src) + 1 : (size_t)n;
    if (cch == 0) return (int)len;
    if ((size_t)cch < len) {
        set_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        char16_t c = src[i];
        if (flags & LCMAP_UPPERCASE) c = fold16(c);
        else if (flags & LCMAP_LOWERCASE) c = lower16(c);
        dst[i] = c;
    }
    return (int)len;
}

BOOL WINAPI GetStringTypeW(DWORD type, LPCWCH src, int n, LPWORD out) {
    if (!src || !out || type != CT_CTYPE1) {
        set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    size_t len = n < 0 ? u16_len(src) + 1 : (size_t)n;
    for (size_t i = 0; i < len; i++) {
        char16_t c = src[i];
        WORD t = 0;
        if (c < 0x80) {
            if (isupper(c)) t |= C1_UPPER | C1_ALPHA;
            if (islower(c)) t |= C1_LOWER | C1_ALPHA;
            if (isdigit(c)) t |= C1_DIGIT;
            if (isspace(c)) t |= C1_SPACE;
            if (ispunct(c)) t |= C1_PUNCT;
            if (iscntrl(c)) t |= C1_CNTRL;
            if (c == ' ' || c == '\t') t |= C1_BLANK;
            if (isxdigit(c)) t |= C1_XDIGIT;
        } else if (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) {
            t |= C1_ALPHA | (lower16(c) == c ? C1_LOWER : C1_UPPER);
        } else if (c >= 0x80 && c < 0xA0) {
            t |= C1_CNTRL;
        } else if (c == 0xA0) {
            t |= C1_SPACE | C1_BLANK;
        } else if (c < 0xC0) {
            t |= C1_PUNCT;
        } else {
            t |= C1_ALPHA;
        }
        if (t) t |= C1_DEFINED;
        out[i] = t;
    }
    return TRUE;
}

BOOL WINAPI GetCPInfo(UINT cp, LPCPINFO info) {
    if (!info) {
        set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    memset(info, 0, sizeof *info);
    info->DefaultChar[0] = '?';
    info->MaxCharSize = utf8_cp(cp) ? 4 : 1;
    if (!utf8_cp(cp) && cp != 1252 && cp != 28591) {
        set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return TRUE;
}

int WINAPI GetLocaleInfoEx(LPCWSTR, LCTYPE type, LPWSTR data, int cch) {
    const char* v = nullptr;
    switch (type & ~LOCALE_NOUSEROVERRIDE) {
    case LOCALE_SNAME: v = "en-US"; break;
    case LOCALE_SISO639LANGNAME: v = "en"; break;
    case LOCALE_SISO3166CTRYNAME: v = "US"; break;
    case LOCALE_SDECIMAL: v = "."; break;
    case LOCALE_STHOUSAND: v = ","; break;
    case LOCALE_IDEFAULTANSICODEPAGE: v = "65001"; break;
    case LOCALE_IDEFAULTCODEPAGE: v = "65001"; break;
    case LOCALE_SENGLISHLANGUAGENAME: v = "English"; break;
    case LOCALE_SENGLISHCOUNTRYNAME: v = "United States"; break;
    case LOCALE_SSHORTDATE: v = "M/d/yyyy"; break;
    case LOCALE_SLONGDATE: v = "dddd, MMMM d, yyyy"; break;
    case LOCALE_STIMEFORMAT: v = "h:mm:ss tt"; break;
    default: break;
    }
    if (!v) {
        set_error(ERROR_INVALID_FLAGS);
        return 0;
    }
    return out_w(v, data, cch);
}

int WINAPI GetDateFormatEx(LPCWSTR, DWORD flags, const SYSTEMTIME* st, LPCWSTR fmt, LPWSTR out, int cch, LPCWSTR) {
    SYSTEMTIME now;
    if (!st) {
        GetLocalTime(&now);
        st = &now;
    }
    std::string pic = fmt ? u16_to_utf8(fmt) : ((flags & DATE_LONGDATE) ? "dddd, MMMM d, yyyy" : "M/d/yyyy");
    return out_w(format_picture(*st, pic, true), out, cch);
}

int WINAPI GetTimeFormatEx(LPCWSTR, DWORD flags, const SYSTEMTIME* st, LPCWSTR fmt, LPWSTR out, int cch) {
    SYSTEMTIME now;
    if (!st) {
        GetLocalTime(&now);
        st = &now;
    }
    std::string pic = fmt ? u16_to_utf8(fmt) : ((flags & TIME_NOSECONDS) ? "h:mm tt" : "h:mm:ss tt");
    return out_w(format_picture(*st, pic, false), out, cch);
}

PVOID WINAPI EncodePointer(PVOID p) { return p; }
PVOID WINAPI DecodePointer(PVOID p) { return p; }

// ---- debug / errors / misc ----

BOOL WINAPI IsDebuggerPresent(void) {
    struct kinfo_proc kp;
    size_t len = sizeof kp;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    if (sysctl(mib, 4, &kp, &len, nullptr, 0) != 0) return FALSE;
    return (kp.kp_proc.p_flag & P_TRACED) != 0;
}

void WINAPI DebugBreak(void) { __builtin_debugtrap(); }

void WINAPI OutputDebugStringA(LPCSTR s) {
    if (!s || !log_enabled(GTA6RT_LOG_INFO)) return;
    std::string m(s);
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    gta6rt_log(GTA6RT_LOG_INFO, "dbg", "%s", m.c_str());
}

void WINAPI OutputDebugStringW(LPCWSTR s) {
    if (s) OutputDebugStringA(u16_to_utf8(s).c_str());
}

static UINT g_error_mode;
UINT WINAPI SetErrorMode(UINT uMode) {
    UINT o = g_error_mode;
    g_error_mode = uMode;
    return o;
}
UINT WINAPI GetErrorMode(void) { return g_error_mode; }

void WINAPI FatalAppExitA(UINT, LPCSTR msg) {
    gta6rt_log(GTA6RT_LOG_ERROR, "os", "FatalAppExit: %s", msg ? msg : "");
    abort();
}

DWORD WINAPI FormatMessageW(DWORD flags, LPCVOID src, DWORD id, DWORD, LPWSTR buf, DWORD n, va_list*) {
    std::string text;
    if (flags & FORMAT_MESSAGE_FROM_STRING) {
        text = src ? u16_to_utf8((LPCWSTR)src) : "";
    } else if (flags & FORMAT_MESSAGE_FROM_SYSTEM) {
        const char* t = error_text(id);
        char tmp[64];
        if (!t) {
            snprintf(tmp, sizeof tmp, "Error %u.", id);
            t = tmp;
        }
        text = std::string(t) + "\r\n";
    } else {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    std::u16string w = utf8_to_u16(text.c_str());
    if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) {
        LPWSTR p = (LPWSTR)malloc((w.size() + 1) * 2);
        memcpy(p, w.data(), w.size() * 2);
        p[w.size()] = 0;
        *(LPWSTR*)buf = p;
        return (DWORD)w.size();
    }
    if (!buf || n < w.size() + 1) {
        set_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    memcpy(buf, w.data(), w.size() * 2);
    buf[w.size()] = 0;
    return (DWORD)w.size();
}

DWORD WINAPI FormatMessageA(DWORD flags, LPCVOID src, DWORD id, DWORD lang, LPSTR buf, DWORD n, va_list* args) {
    std::u16string srcw;
    if ((flags & FORMAT_MESSAGE_FROM_STRING) && src) {
        srcw = utf8_to_u16((const char*)src);
        src = srcw.c_str();
    }
    LPWSTR w = nullptr;
    DWORD r = FormatMessageW(flags | FORMAT_MESSAGE_ALLOCATE_BUFFER, src, id, lang, (LPWSTR)&w, 0, args);
    if (!r) return 0;
    std::string s = u16_to_utf8(w);
    free(w);
    if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) {
        char* p = strdup(s.c_str());
        *(LPSTR*)buf = p;
        return (DWORD)s.size();
    }
    if (!buf || n < s.size() + 1) {
        set_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    memcpy(buf, s.c_str(), s.size() + 1);
    return (DWORD)s.size();
}

HANDLE WINAPI PowerCreateRequest(PREASON_CONTEXT) {
    uint64_t h = 0;
    gta6rt_handle_alloc(GTA6RT_HANDLE_OTHER, nullptr, nullptr, &h);
    return HV(h);
}
BOOL WINAPI PowerSetRequest(HANDLE, POWER_REQUEST_TYPE) { return TRUE; }
BOOL WINAPI PowerClearRequest(HANDLE, POWER_REQUEST_TYPE) { return TRUE; }

static EXECUTION_STATE g_exec_state = ES_CONTINUOUS;
EXECUTION_STATE WINAPI SetThreadExecutionState(EXECUTION_STATE es) {
    EXECUTION_STATE o = g_exec_state;
    g_exec_state = es;
    return o;
}

BOOL WINAPI OpenProcessToken(HANDLE, DWORD, PHANDLE TokenHandle) {
    uint64_t h = 0;
    gta6rt_handle_alloc(GTA6RT_HANDLE_OTHER, nullptr, nullptr, &h);
    *TokenHandle = HV(h);
    return TRUE;
}

BOOL WINAPI OpenThreadToken(HANDLE, DWORD, BOOL, PHANDLE) {
    set_error(ERROR_NO_TOKEN);
    return FALSE;
}

BOOL WINAPI LookupPrivilegeValueW(LPCWSTR, LPCWSTR lpName, PLUID lpLuid) {
    if (lpLuid) {
        lpLuid->LowPart = lpName && cmp16(lpName, -1, u"SeLockMemoryPrivilege", -1, true) == CSTR_EQUAL ? 4 : 20;
        lpLuid->HighPart = 0;
    }
    return TRUE;
}

BOOL WINAPI AdjustTokenPrivileges(HANDLE, BOOL, PTOKEN_PRIVILEGES, DWORD, PTOKEN_PRIVILEGES prev, PDWORD ret) {
    if (prev) prev->PrivilegeCount = 0;
    if (ret) *ret = 0;
    set_error(ERROR_SUCCESS);
    return TRUE;
}

BOOL WINAPI PrivilegeCheck(HANDLE, PPRIVILEGE_SET, LPBOOL pfResult) {
    if (pfResult) *pfResult = TRUE;
    return TRUE;
}

// ---- module table ----

gta6rt_status gta6rt_module_register(const gta6rt_module_desc* desc, gta6rt_module** out) {
    if (!desc || !desc->name) return GTA6RT_E_INVALID_ARG;
    ModEntry* m = new ModEntry();
    m->d = *desc;
    m->name = desc->name;
    m->path = desc->path_utf8 ? desc->path_utf8 : "";
    m->key = mod_key(desc->name);
    m->d.name = m->name.c_str();
    m->d.path_utf8 = desc->path_utf8 ? m->path.c_str() : nullptr;
    pthread_mutex_lock(&g_mod_mx);
    if (!g_mods) g_mods = new std::vector<ModEntry*>();
    for (ModEntry* e : *g_mods)
        if (e->key == m->key) {
            pthread_mutex_unlock(&g_mod_mx);
            delete m;
            return GTA6RT_E_ALREADY_EXISTS;
        }
    g_mods->push_back(m);
    pthread_mutex_unlock(&g_mod_mx);
    if (out) *out = (gta6rt_module*)m;
    return GTA6RT_OK;
}

gta6rt_status gta6rt_module_unregister(gta6rt_module* mod) {
    ModEntry* m = (ModEntry*)mod;
    pthread_mutex_lock(&g_mod_mx);
    bool found = false;
    if (g_mods)
        for (auto it = g_mods->begin(); it != g_mods->end(); ++it)
            if (*it == m) {
                g_mods->erase(it);
                found = true;
                break;
            }
    if (g_main == m) g_main = nullptr;
    pthread_mutex_unlock(&g_mod_mx);
    if (!found) return GTA6RT_E_NOT_FOUND;
    delete m;
    return GTA6RT_OK;
}

gta6rt_module* gta6rt_module_find_by_name(const char* name_utf8) {
    std::string k = mod_key(name_utf8);
    pthread_mutex_lock(&g_mod_mx);
    ModEntry* r = nullptr;
    if (g_mods)
        for (ModEntry* m : *g_mods)
            if (m->key == k) {
                r = m;
                break;
            }
    pthread_mutex_unlock(&g_mod_mx);
    return (gta6rt_module*)r;
}

gta6rt_module* gta6rt_module_find_by_address(gta6rt_gva addr) {
    pthread_mutex_lock(&g_mod_mx);
    ModEntry* r = nullptr;
    if (g_mods)
        for (ModEntry* m : *g_mods)
            if (addr >= m->d.base && addr < m->d.base + (m->d.size ? m->d.size : 1)) {
                r = m;
                break;
            }
    pthread_mutex_unlock(&g_mod_mx);
    return (gta6rt_module*)r;
}

gta6rt_module* gta6rt_module_find_by_base(gta6rt_gva base) {
    pthread_mutex_lock(&g_mod_mx);
    ModEntry* r = nullptr;
    if (g_mods)
        for (ModEntry* m : *g_mods)
            if (m->d.base == base) {
                r = m;
                break;
            }
    pthread_mutex_unlock(&g_mod_mx);
    return (gta6rt_module*)r;
}

const gta6rt_module_desc* gta6rt_module_info(const gta6rt_module* m) { return m ? &((const ModEntry*)m)->d : nullptr; }

gta6rt_gva gta6rt_module_get_export(gta6rt_module* mod, const char* name, uint32_t ordinal) {
    ModEntry* m = (ModEntry*)mod;
    if (!m || !m->d.get_export) return 0;
    return m->d.get_export(m->d.user, name, ordinal);
}

gta6rt_module* gta6rt_module_at(size_t index) {
    pthread_mutex_lock(&g_mod_mx);
    ModEntry* r = g_mods && index < g_mods->size() ? (*g_mods)[index] : nullptr;
    pthread_mutex_unlock(&g_mod_mx);
    return (gta6rt_module*)r;
}

gta6rt_status gta6rt_module_set_load_hook(gta6rt_module_load_fn fn, void* user) {
    g_load_user = user;
    g_load_hook = fn;
    return GTA6RT_OK;
}

gta6rt_status gta6rt_module_set_main(gta6rt_module* m) {
    pthread_mutex_lock(&g_mod_mx);
    g_main = (ModEntry*)m;
    pthread_mutex_unlock(&g_mod_mx);
    return GTA6RT_OK;
}

HMODULE WINAPI GetModuleHandleA(LPCSTR name) {
    if (!name) {
        pthread_mutex_lock(&g_mod_mx);
        ModEntry* m = main_module_locked();
        pthread_mutex_unlock(&g_mod_mx);
        return m ? (HMODULE)(uintptr_t)m->d.base : (HMODULE)_dyld_get_image_header(0);
    }
    gta6rt_module* m = gta6rt_module_find_by_name(name);
    if (!m) {
        set_error(ERROR_MOD_NOT_FOUND);
        return nullptr;
    }
    return (HMODULE)(uintptr_t)((ModEntry*)m)->d.base;
}

HMODULE WINAPI GetModuleHandleW(LPCWSTR name) {
    if (!name) return GetModuleHandleA(nullptr);
    return GetModuleHandleA(u16_to_utf8(name).c_str());
}

BOOL WINAPI GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE* out) {
    HMODULE h = nullptr;
    if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
        gta6rt_module* m = gta6rt_module_find_by_address((gta6rt_gva)(uintptr_t)name);
        if (m) h = (HMODULE)(uintptr_t)((ModEntry*)m)->d.base;
    } else {
        h = GetModuleHandleW(name);
    }
    if (out) *out = h;
    if (!h) {
        set_error(ERROR_MOD_NOT_FOUND);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE* out) {
    if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) return GetModuleHandleExW(flags, (LPCWSTR)name, out);
    std::u16string w = utf8_to_u16(name);
    return GetModuleHandleExW(flags, name ? w.c_str() : nullptr, out);
}

DWORD WINAPI GetModuleFileNameW(HMODULE h, LPWSTR buf, DWORD n) {
    std::string path;
    pthread_mutex_lock(&g_mod_mx);
    ModEntry* m = find_handle_locked(h);
    if (m) path = m->path.empty() ? m->name : m->path;
    pthread_mutex_unlock(&g_mod_mx);
    if (!m) {
        if (h && h != (HMODULE)_dyld_get_image_header(0)) {
            set_error(ERROR_MOD_NOT_FOUND);
            return 0;
        }
        char tmp[PATH_MAX];
        uint32_t sz = sizeof tmp;
        path = _NSGetExecutablePath(tmp, &sz) == 0 ? tmp : "gta6rt";
    }
    std::u16string w = path.find('/') != std::string::npos ? host_to_win_path(path) : utf8_to_u16(path.c_str());
    if (!buf || n == 0) {
        set_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    if (w.size() + 1 > n) {
        memcpy(buf, w.data(), (n - 1) * 2);
        buf[n - 1] = 0;
        set_error(ERROR_INSUFFICIENT_BUFFER);
        return n;
    }
    memcpy(buf, w.data(), w.size() * 2);
    buf[w.size()] = 0;
    set_error(ERROR_SUCCESS);
    return (DWORD)w.size();
}

DWORD WINAPI GetModuleFileNameA(HMODULE h, LPSTR buf, DWORD n) {
    WCHAR tmp[4096];
    DWORD r = GetModuleFileNameW(h, tmp, 4096);
    if (!r) return 0;
    std::string s = u16_to_utf8(tmp);
    if (!buf || n == 0) {
        set_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    if (s.size() + 1 > n) {
        memcpy(buf, s.data(), n - 1);
        buf[n - 1] = 0;
        set_error(ERROR_INSUFFICIENT_BUFFER);
        return n;
    }
    memcpy(buf, s.c_str(), s.size() + 1);
    return (DWORD)s.size();
}

HMODULE WINAPI LoadLibraryExA(LPCSTR name, HANDLE, DWORD flags) {
    if (!name) {
        set_error(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    ModEntry* m = (ModEntry*)gta6rt_module_find_by_name(name);
    if (m) {
        m->loads.fetch_add(1);
        return (HMODULE)(uintptr_t)m->d.base;
    }
    gta6rt_module_load_fn hook = g_load_hook;
    if (hook) {
        m = (ModEntry*)hook(name, flags, g_load_user);
        if (m) return (HMODULE)(uintptr_t)m->d.base;
    }
    OS_LOG(GTA6RT_LOG_INFO, "LoadLibrary(%s, %#x): not found", name, flags);
    set_error(ERROR_MOD_NOT_FOUND);
    return nullptr;
}

HMODULE WINAPI LoadLibraryExW(LPCWSTR name, HANDLE f, DWORD flags) {
    if (!name) {
        set_error(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    return LoadLibraryExA(u16_to_utf8(name).c_str(), f, flags);
}

HMODULE WINAPI LoadLibraryA(LPCSTR name) { return LoadLibraryExA(name, nullptr, 0); }
HMODULE WINAPI LoadLibraryW(LPCWSTR name) { return LoadLibraryExW(name, nullptr, 0); }

BOOL WINAPI FreeLibrary(HMODULE h) {
    pthread_mutex_lock(&g_mod_mx);
    ModEntry* m = h ? find_handle_locked(h) : nullptr;
    if (m && m->loads.load() > 1) m->loads.fetch_sub(1);
    pthread_mutex_unlock(&g_mod_mx);
    if (!m) {
        set_error(ERROR_MOD_NOT_FOUND);
        return FALSE;
    }
    return TRUE;
}

void WINAPI FreeLibraryAndExitThread(HMODULE h, DWORD code) {
    FreeLibrary(h);
    ExitThread(code);
}

FARPROC WINAPI GetProcAddress(HMODULE h, LPCSTR name) {
    pthread_mutex_lock(&g_mod_mx);
    ModEntry* m = find_handle_locked(h);
    pthread_mutex_unlock(&g_mod_mx);
    if (!m) {
        set_error(ERROR_MOD_NOT_FOUND);
        return nullptr;
    }
    gta6rt_gva a = IS_INTRESOURCE(name) ? gta6rt_module_get_export((gta6rt_module*)m, nullptr, (uint32_t)(uintptr_t)name)
                                        : gta6rt_module_get_export((gta6rt_module*)m, name, 0);
    if (!a) {
        if (!IS_INTRESOURCE(name)) OS_LOG(GTA6RT_LOG_DEBUG, "GetProcAddress(%s, %s): not found", m->name.c_str(), name);
        set_error(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }
    return (FARPROC)(uintptr_t)a;
}

BOOL WINAPI DisableThreadLibraryCalls(HMODULE) { return TRUE; }
BOOL WINAPI SetDefaultDllDirectories(DWORD) { return TRUE; }

HRSRC WINAPI FindResourceW(HMODULE, LPCWSTR, LPCWSTR) {
    set_error(1812 /* ERROR_RESOURCE_DATA_NOT_FOUND */);
    return nullptr;
}
HGLOBAL WINAPI LoadResource(HMODULE, HRSRC) { return nullptr; }
LPVOID WINAPI LockResource(HGLOBAL h) { return h; }
DWORD WINAPI SizeofResource(HMODULE, HRSRC) { return 0; }

HRESULT WINAPI WerRegisterFile(PCWSTR pwzFile, WER_REGISTER_FILE_TYPE, DWORD) {
    OS_LOG(GTA6RT_LOG_DEBUG, "WerRegisterFile(%s)", u16_to_utf8(pwzFile).c_str());
    return S_OK;
}

HRESULT WINAPI WerUnregisterFile(PCWSTR) { return S_OK; }

// ---- psapi ----

BOOL WINAPI GetProcessMemoryInfo(HANDLE Process, PPROCESS_MEMORY_COUNTERS c, DWORD cb) {
    if (H(Process) != kPseudoProcess || !c || cb < sizeof(PROCESS_MEMORY_COUNTERS)) {
        set_error(H(Process) != kPseudoProcess ? ERROR_INVALID_HANDLE : ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    task_vm_info_data_t vi;
    mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
    memset(&vi, 0, sizeof vi);
    task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &cnt);
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    memset(c, 0, cb >= sizeof(PROCESS_MEMORY_COUNTERS_EX) ? sizeof(PROCESS_MEMORY_COUNTERS_EX) : sizeof *c);
    c->cb = cb >= sizeof(PROCESS_MEMORY_COUNTERS_EX) ? sizeof(PROCESS_MEMORY_COUNTERS_EX) : sizeof *c;
    c->PageFaultCount = (DWORD)(ru.ru_minflt + ru.ru_majflt);
    c->WorkingSetSize = vi.resident_size;
    c->PeakWorkingSetSize = vi.resident_size_peak;
    c->PagefileUsage = vi.phys_footprint;
    c->PeakPagefileUsage = vi.ledger_phys_footprint_peak ? vi.ledger_phys_footprint_peak : vi.phys_footprint;
    if (cb >= sizeof(PROCESS_MEMORY_COUNTERS_EX)) ((PPROCESS_MEMORY_COUNTERS_EX)c)->PrivateUsage = vi.phys_footprint;
    return TRUE;
}

BOOL WINAPI K32GetProcessMemoryInfo(HANDLE Process, PPROCESS_MEMORY_COUNTERS c, DWORD cb) {
    return GetProcessMemoryInfo(Process, c, cb);
}

BOOL WINAPI GetModuleInformation(HANDLE hProcess, HMODULE hModule, LPMODULEINFO info, DWORD cb) {
    if (H(hProcess) != kPseudoProcess || !info || cb < sizeof *info) {
        set_error(H(hProcess) != kPseudoProcess ? ERROR_INVALID_HANDLE : ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    pthread_mutex_lock(&g_mod_mx);
    ModEntry* m = find_handle_locked(hModule);
    if (m) {
        info->lpBaseOfDll = (LPVOID)(uintptr_t)m->d.base;
        info->SizeOfImage = (DWORD)m->d.size;
        info->EntryPoint = nullptr;
    }
    pthread_mutex_unlock(&g_mod_mx);
    if (!m) {
        set_error(ERROR_MOD_NOT_FOUND);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI EnumProcessModules(HANDLE hProcess, HMODULE* mods, DWORD cb, LPDWORD needed) {
    if (H(hProcess) != kPseudoProcess || !needed) {
        set_error(H(hProcess) != kPseudoProcess ? ERROR_INVALID_HANDLE : ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    pthread_mutex_lock(&g_mod_mx);
    std::vector<HMODULE> v;
    ModEntry* main = main_module_locked();
    if (main) v.push_back((HMODULE)(uintptr_t)main->d.base);
    if (g_mods)
        for (ModEntry* m : *g_mods)
            if (m != main) v.push_back((HMODULE)(uintptr_t)m->d.base);
    pthread_mutex_unlock(&g_mod_mx);
    *needed = (DWORD)(v.size() * sizeof(HMODULE));
    size_t n = cb / sizeof(HMODULE);
    for (size_t i = 0; i < v.size() && i < n && mods; i++) mods[i] = v[i];
    return TRUE;
}

} // extern "C"
