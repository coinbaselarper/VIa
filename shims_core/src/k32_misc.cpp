// The kernel32-level imports of the exe that the spike classified as trivial: last error, time, process/thread
// identity, strings, locale, system information. Thin wrappers over the os.h primitives and libc; every function is a
// guest ABI entry (ms_abi on x86_64, frame adapter on arm64). Registered under kernel32.dll; the api-ms-win-core-*
// sets resolve through the family fallback in registry.cpp.
#include <windows.h>

#include <pthread.h>
#include <sys/sysctl.h>
#include <sys/mount.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <gta6rt/os.h>

#include "file_system.h"
#include "k32_shared.h"
#include "process_shim.h"
#include "shims.h"
#include "util.h"

namespace rt::k32 {
namespace {

constexpr uint32_t kErrorSuccess = 0, kErrorFileNotFound = 2, kErrorPathNotFound = 3, kErrorInvalidHandle = 6,
                   kErrorInvalidParameter = 87, kErrorInsufficientBuffer = 122, kErrorNoUnicodeTranslation = 1113;

bool is_current_process(HANDLE h) {
    const auto v = reinterpret_cast<intptr_t>(h);
    return v == -1 || v == static_cast<intptr_t>(getpid());
}

uint64_t sysctl_u64(const char* name, uint64_t fallback) {
    uint64_t v = 0;
    size_t n = sizeof v;
    if (sysctlbyname(name, &v, &n, nullptr, 0) != 0) return fallback;
    if (n == 4) return static_cast<uint32_t>(v);
    return v;
}

uint32_t logical_cpus() {
    static const uint32_t n = static_cast<uint32_t>(sysctl_u64("hw.logicalcpu", 1));
    return n ? n : 1;
}
uint32_t physical_cpus() {
    static const uint32_t n = static_cast<uint32_t>(sysctl_u64("hw.physicalcpu", logical_cpus()));
    return n ? n : 1;
}

// ---- last error, debug, process identity

DWORD WINAPI w_GetLastError() { return gta6rt_last_error_get(); }
void WINAPI w_SetLastError(DWORD e) { gta6rt_last_error_set(e); }

BOOL WINAPI w_IsDebuggerPresent() {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    struct kinfo_proc info;
    size_t size = sizeof info;
    std::memset(&info, 0, sizeof info);
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0) return FALSE;
    return (info.kp_proc.p_flag & P_TRACED) != 0;
}

void WINAPI w_OutputDebugStringA(const char* text) {
    if (text) gta6rt_log(GTA6RT_LOG_INFO, "debug", "%s", text);
}

HANDLE WINAPI w_GetCurrentProcess() { return reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1)); }
DWORD WINAPI w_GetCurrentProcessId() { return static_cast<DWORD>(getpid()); }

DWORD WINAPI w_GetCurrentThreadId() {
    if (gta6rt_thread* t = gta6rt_thread_current()) return gta6rt_thread_id(t);
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    return static_cast<DWORD>(tid);
}

DWORD WINAPI w_GetProcessId(HANDLE h) {
    if (is_current_process(h)) return static_cast<DWORD>(getpid());
    gta6rt_last_error_set(kErrorInvalidHandle);
    return 0;
}

BOOL WINAPI w_TerminateProcess(HANDLE h, UINT code) {
    if (!is_current_process(h)) {
        gta6rt_last_error_set(kErrorInvalidHandle);
        return FALSE;
    }
    process::abort_run(static_cast<int>(code));
}

void WINAPI w_GetStartupInfoW(STARTUPINFOW* info) {
    if (!info) return;
    std::memset(info, 0, sizeof *info);
    info->cb = sizeof *info;
}

BOOL WINAPI w_IsProcessorFeaturePresent(DWORD feature) {
    switch (feature) {
    case 2: case 3: case 6: case 8: case 9: case 10: case 12: case 13: case 14: case 17: case 20: case 23:
    case 36: case 37: case 38: case 39: case 40:
        return TRUE;
    default:
        return FALSE;
    }
}

void WINAPI w_GetCurrentThreadStackLimits(ULONG_PTR* low, ULONG_PTR* high) {
    uint64_t base = 0, limit = 0;
    if (gta6rt_thread* t = gta6rt_thread_current()) gta6rt_thread_stack(t, &base, &limit);
    if (!base) {
        const pthread_t self = pthread_self();
        base = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(self));
        limit = base - pthread_get_stacksize_np(self);
    }
    if (low) *low = limit;
    if (high) *high = base;
}

std::mutex g_prio_mutex;
std::map<uintptr_t, int> g_priority;

gta6rt_thread* thread_of(HANDLE h) { return rt::k32::thread_of_handle(h); }

BOOL WINAPI w_SetThreadPriority(HANDLE h, int priority) {
    gta6rt_thread* t = thread_of(h);
    if (!t) {
        gta6rt_last_error_set(kErrorInvalidHandle);
        return FALSE;
    }
    if (priority < -15 || priority > 15) {
        gta6rt_last_error_set(kErrorInvalidParameter);
        return FALSE;
    }
    gta6rt_thread_set_priority(t, priority);
    std::lock_guard lock(g_prio_mutex);
    g_priority[reinterpret_cast<uintptr_t>(t)] = priority;
    return TRUE;
}

int WINAPI w_GetThreadPriority(HANDLE h) {
    gta6rt_thread* t = thread_of(h);
    if (!t) {
        gta6rt_last_error_set(kErrorInvalidHandle);
        return 0x7fffffff;
    }
    std::lock_guard lock(g_prio_mutex);
    auto it = g_priority.find(reinterpret_cast<uintptr_t>(t));
    return it == g_priority.end() ? 0 : it->second;
}

BOOL WINAPI w_SetProcessPriorityBoost(HANDLE h, BOOL disable) {
    (void)disable;
    if (!is_current_process(h)) {
        gta6rt_last_error_set(kErrorInvalidHandle);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI w_GetThreadGroupAffinity(HANDLE h, GROUP_AFFINITY* out) {
    if (!out) {
        gta6rt_last_error_set(kErrorInvalidParameter);
        return FALSE;
    }
    if (!thread_of(h)) {
        gta6rt_last_error_set(kErrorInvalidHandle);
        return FALSE;
    }
    std::memset(out, 0, sizeof *out);
    out->Mask = affinity_mask();
    out->Group = 0;
    return TRUE;
}

DWORD WINAPI w_RegisterAppConstrainedChangeNotification(void* routine, void* context, void** registration) {
    (void)context;
    if (!routine || !registration) return kErrorInvalidParameter;
    static std::atomic<uintptr_t> next{0x1000};
    *registration = reinterpret_cast<void*>(next.fetch_add(0x10));
    return kErrorSuccess;
}
void WINAPI w_UnregisterAppConstrainedChangeNotification(void* registration) { (void)registration; }

HRESULT WINAPI w_WerRegisterFile(const char16_t* file, int type, DWORD flags) {
    (void)type;
    (void)flags;
    RTLOG(INFO, "WerRegisterFile(%s) accepted; no crash report is produced", narrow(file).c_str());
    return 0;
}

BOOL WINAPI w_SetNamedPipeHandleState(HANDLE h, DWORD* mode, DWORD* max_collection, DWORD* timeout) {
    (void)h; (void)mode; (void)max_collection; (void)timeout;
    RTLOG(WARN, "SetNamedPipeHandleState: named pipes are not available on this host");
    gta6rt_last_error_set(kErrorInvalidHandle);
    return FALSE;
}

BOOL WINAPI w_WaitNamedPipeW(const char16_t* name, DWORD timeout) {
    (void)timeout;
    RTLOG(WARN, "WaitNamedPipeW(%s): named pipes are not available on this host", narrow(name).c_str());
    gta6rt_last_error_set(kErrorFileNotFound);
    return FALSE;
}

// ---- memory

void* WINAPI w_LocalAlloc(UINT flags, size_t size) {
    void* p = (flags & 0x40) ? std::calloc(1, size ? size : 1) : std::malloc(size ? size : 1);
    if (!p) gta6rt_last_error_set(8);
    return p;
}
void* WINAPI w_LocalFree(void* p) {
    std::free(p);
    return nullptr;
}

void WINAPI w_InitializeSListHead(SLIST_HEADER* head) {
    if (head) std::memset(head, 0, sizeof *head);
}

BOOL WINAPI w_GlobalMemoryStatusEx(MEMORYSTATUSEX* ms) {
    if (!ms || ms->dwLength < sizeof(MEMORYSTATUSEX)) {
        gta6rt_last_error_set(kErrorInvalidParameter);
        return FALSE;
    }
    const uint64_t total = sysctl_u64("hw.memsize", 16ull << 30);
    const uint64_t page = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    const uint64_t free_pages = sysctl_u64("vm.page_free_count", 0) + sysctl_u64("vm.page_speculative_count", 0) +
                                sysctl_u64("vm.page_purgeable_count", 0);
    uint64_t avail = free_pages * page;
    if (avail > total) avail = total;
    ms->dwMemoryLoad = static_cast<DWORD>(total ? 100 - (avail * 100 / total) : 0);
    ms->ullTotalPhys = total;
    ms->ullAvailPhys = avail;
    ms->ullTotalPageFile = total;
    ms->ullAvailPageFile = avail;
    ms->ullTotalVirtual = 0x7FFFFFFE0000ull;
    ms->ullAvailVirtual = 0x7FFFFFFE0000ull - (total - avail);
    ms->ullAvailExtendedVirtual = 0;
    return TRUE;
}

BOOL WINAPI w_GetLogicalProcessorInformation(SYSTEM_LOGICAL_PROCESSOR_INFORMATION* buf, DWORD* length) {
    if (!length) {
        gta6rt_last_error_set(kErrorInvalidParameter);
        return FALSE;
    }
    const uint32_t logical = logical_cpus() > 64 ? 64 : logical_cpus();
    const uint32_t physical = physical_cpus() > logical ? logical : physical_cpus();
    const uint32_t threads_per_core = physical ? (logical + physical - 1) / physical : 1;
    const size_t entries = physical + 2;
    const DWORD need = static_cast<DWORD>(entries * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (!buf || *length < need) {
        *length = need;
        gta6rt_last_error_set(kErrorInsufficientBuffer);
        return FALSE;
    }
    std::memset(buf, 0, need);
    size_t i = 0;
    uint32_t bit = 0;
    for (uint32_t core = 0; core < physical; ++core) {
        uint64_t mask = 0;
        for (uint32_t t = 0; t < threads_per_core && bit < logical; ++t, ++bit) mask |= 1ull << bit;
        buf[i].ProcessorMask = mask;
        buf[i].Relationship = RelationProcessorCore;
        buf[i].ProcessorCore.Flags = threads_per_core > 1 ? 1 : 0;
        ++i;
    }
    buf[i].ProcessorMask = affinity_mask();
    buf[i].Relationship = RelationNumaNode;
    buf[i].NumaNode.NodeNumber = 0;
    ++i;
    buf[i].ProcessorMask = affinity_mask();
    buf[i].Relationship = RelationProcessorPackage;
    *length = need;
    return TRUE;
}

// ---- time

constexpr uint64_t kTicksPerSecond = 10000000ull;
constexpr uint64_t kEpochDiff = 11644473600ull;

int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civil_from_days(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}

uint64_t ft_to_u64(const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }
void u64_to_ft(uint64_t v, FILETIME* f) {
    f->dwLowDateTime = static_cast<DWORD>(v);
    f->dwHighDateTime = static_cast<DWORD>(v >> 32);
}

void fill_systemtime(uint64_t ft, SYSTEMTIME* st) {
    const uint64_t total_ms = ft / 10000;
    const uint64_t days = total_ms / 86400000ull;
    const uint64_t ms_of_day = total_ms % 86400000ull;
    int64_t y;
    unsigned m, d;
    civil_from_days(static_cast<int64_t>(days) - static_cast<int64_t>(kEpochDiff / 86400), y, m, d);
    st->wYear = static_cast<WORD>(y);
    st->wMonth = static_cast<WORD>(m);
    st->wDay = static_cast<WORD>(d);
    st->wDayOfWeek = static_cast<WORD>((days + 1) % 7);
    st->wHour = static_cast<WORD>(ms_of_day / 3600000);
    st->wMinute = static_cast<WORD>(ms_of_day / 60000 % 60);
    st->wSecond = static_cast<WORD>(ms_of_day / 1000 % 60);
    st->wMilliseconds = static_cast<WORD>(ms_of_day % 1000);
}

int64_t local_offset_seconds(uint64_t utc_ft) {
    const time_t t = static_cast<time_t>(utc_ft / kTicksPerSecond - kEpochDiff);
    tm local;
    localtime_r(&t, &local);
    return local.tm_gmtoff;
}

BOOL WINAPI w_FileTimeToSystemTime(const FILETIME* ft, SYSTEMTIME* st) {
    if (!ft || !st || (ft->dwHighDateTime & 0x80000000u)) {
        gta6rt_last_error_set(kErrorInvalidParameter);
        return FALSE;
    }
    fill_systemtime(ft_to_u64(*ft), st);
    return TRUE;
}

BOOL WINAPI w_SystemTimeToFileTime(const SYSTEMTIME* st, FILETIME* ft) {
    if (!st || !ft || st->wMonth < 1 || st->wMonth > 12 || st->wDay < 1 || st->wDay > 31 || st->wHour > 23 ||
        st->wMinute > 59 || st->wSecond > 59 || st->wMilliseconds > 999 || st->wYear < 1601 || st->wYear > 30827) {
        gta6rt_last_error_set(kErrorInvalidParameter);
        return FALSE;
    }
    const int64_t days = days_from_civil(st->wYear, st->wMonth, st->wDay) + static_cast<int64_t>(kEpochDiff / 86400);
    const uint64_t ms = static_cast<uint64_t>(days) * 86400000ull + st->wHour * 3600000ull + st->wMinute * 60000ull +
                        st->wSecond * 1000ull + st->wMilliseconds;
    u64_to_ft(ms * 10000, ft);
    return TRUE;
}

void WINAPI w_GetSystemTimeAsFileTime(FILETIME* ft) { u64_to_ft(gta6rt_time_filetime_utc(), ft); }

void WINAPI w_GetSystemTime(SYSTEMTIME* st) { fill_systemtime(gta6rt_time_filetime_utc(), st); }

void WINAPI w_GetLocalTime(SYSTEMTIME* st) {
    const uint64_t utc = gta6rt_time_filetime_utc();
    fill_systemtime(static_cast<uint64_t>(static_cast<int64_t>(utc) + local_offset_seconds(utc) * static_cast<int64_t>(kTicksPerSecond)), st);
}

BOOL WINAPI w_FileTimeToLocalFileTime(const FILETIME* in, FILETIME* out) {
    if (!in || !out) {
        gta6rt_last_error_set(kErrorInvalidParameter);
        return FALSE;
    }
    const uint64_t utc = ft_to_u64(*in);
    u64_to_ft(static_cast<uint64_t>(static_cast<int64_t>(utc) + local_offset_seconds(utc) * static_cast<int64_t>(kTicksPerSecond)), out);
    return TRUE;
}

BOOL WINAPI w_LocalFileTimeToFileTime(const FILETIME* in, FILETIME* out) {
    if (!in || !out) {
        gta6rt_last_error_set(kErrorInvalidParameter);
        return FALSE;
    }
    const uint64_t local = ft_to_u64(*in);
    int64_t off = local_offset_seconds(local);
    off = local_offset_seconds(static_cast<uint64_t>(static_cast<int64_t>(local) - off * static_cast<int64_t>(kTicksPerSecond)));
    u64_to_ft(static_cast<uint64_t>(static_cast<int64_t>(local) - off * static_cast<int64_t>(kTicksPerSecond)), out);
    return TRUE;
}

ULONGLONG WINAPI w_GetTickCount64() { return gta6rt_time_monotonic_ns() / 1000000ull; }

void WINAPI w_Sleep(DWORD ms) {
    if (ms == 0) {
        gta6rt_thread_yield();
        return;
    }
    if (ms == 0xFFFFFFFFu) {
        for (;;) gta6rt_sleep_ns(1000000000ull * 3600);
    }
    gta6rt_sleep_ns(static_cast<uint64_t>(ms) * 1000000ull);
}

BOOL WINAPI w_QueryPerformanceCounter(LARGE_INTEGER* v) {
    if (!v) {
        gta6rt_last_error_set(kErrorInvalidParameter);
        return FALSE;
    }
    v->QuadPart = static_cast<LONGLONG>(gta6rt_time_qpc());
    return TRUE;
}

BOOL WINAPI w_QueryPerformanceFrequency(LARGE_INTEGER* v) {
    if (!v) {
        gta6rt_last_error_set(kErrorInvalidParameter);
        return FALSE;
    }
    v->QuadPart = static_cast<LONGLONG>(gta6rt_time_qpc_frequency());
    return TRUE;
}

// ---- code pages

enum class Cp { Utf8, Latin1, Cp1252, Unsupported };

Cp codepage_kind(UINT cp) {
    switch (cp) {
    case 0: case 1: case 2: case 3: case 65001: return Cp::Utf8;
    case 28591: return Cp::Latin1;
    case 1252: return Cp::Cp1252;
    default: return Cp::Unsupported;
    }
}

const char16_t kCp1252High[32] = {0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                  0x2039, 0x0152, 0x8D, 0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                  0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};

int WINAPI w_MultiByteToWideChar(UINT cp, DWORD flags, const char* src, int cb, char16_t* dst, int cch) {
    if (!src || cb == 0 || cb < -1 || cch < 0 || (dst == nullptr && cch != 0)) {
        gta6rt_last_error_set(kErrorInvalidParameter);
        return 0;
    }
    const Cp kind = codepage_kind(cp);
    if (kind == Cp::Unsupported) {
        RTLOG(WARN, "MultiByteToWideChar: code page %u is not supported", cp);
        gta6rt_last_error_set(kErrorInvalidParameter);
        return 0;
    }
    const size_t length = cb == -1 ? std::strlen(src) + 1 : static_cast<size_t>(cb);
    std::u16string out;
    if (kind == Cp::Utf8) {
        if (flags & 0x8) {
            // MB_ERR_INVALID_CHARS: validate first
            const std::u16string probe = widen(src, length);
            for (char16_t c : probe)
                if (c == 0xFFFD) {
                    bool literal = false;
                    for (size_t i = 0; i + 2 < length; ++i)
                        if (static_cast<uint8_t>(src[i]) == 0xEF && static_cast<uint8_t>(src[i + 1]) == 0xBF &&
                            static_cast<uint8_t>(src[i + 2]) == 0xBD)
                            literal = true;
                    if (!literal) {
                        gta6rt_last_error_set(kErrorNoUnicodeTranslation);
                        return 0;
                    }
                }
            out = probe;
        } else {
            out = widen(src, length);
        }
    } else {
        for (size_t i = 0; i < length; ++i) {
            const uint8_t b = static_cast<uint8_t>(src[i]);
            out.push_back((kind == Cp::Cp1252 && b >= 0x80 && b < 0xA0) ? kCp1252High[b - 0x80] : b);
        }
    }
    if (cch == 0) return static_cast<int>(out.size());
    if (out.size() > static_cast<size_t>(cch)) {
        gta6rt_last_error_set(kErrorInsufficientBuffer);
        return 0;
    }
    std::memcpy(dst, out.data(), out.size() * sizeof(char16_t));
    return static_cast<int>(out.size());
}

int WINAPI w_WideCharToMultiByte(UINT cp, DWORD flags, const char16_t* src, int cch, char* dst, int cb,
                                 const char* default_char, BOOL* used_default) {
    if (!src || cch == 0 || cch < -1 || cb < 0 || (dst == nullptr && cb != 0)) {
        gta6rt_last_error_set(kErrorInvalidParameter);
        return 0;
    }
    const Cp kind = codepage_kind(cp);
    if (kind == Cp::Unsupported) {
        RTLOG(WARN, "WideCharToMultiByte: code page %u is not supported", cp);
        gta6rt_last_error_set(kErrorInvalidParameter);
        return 0;
    }
    if (kind == Cp::Utf8 && (default_char || used_default)) {
        gta6rt_last_error_set(kErrorInvalidParameter);
        return 0;
    }
    if (used_default) *used_default = FALSE;
    const size_t length = cch == -1 ? u16len(src) + 1 : static_cast<size_t>(cch);
    std::string out;
    if (kind == Cp::Utf8) {
        if (flags & 0x80) {
            for (size_t i = 0; i < length; ++i) {
                const char16_t c = src[i];
                if (c >= 0xD800 && c <= 0xDBFF && i + 1 < length && src[i + 1] >= 0xDC00 && src[i + 1] <= 0xDFFF) ++i;
                else if (c >= 0xD800 && c <= 0xDFFF) {
                    gta6rt_last_error_set(kErrorNoUnicodeTranslation);
                    return 0;
                }
            }
        }
        out = narrow(src, length);
    } else {
        const char def = default_char ? *default_char : '?';
        for (size_t i = 0; i < length; ++i) {
            const char16_t c = src[i];
            if (c < 0x80 || (kind == Cp::Latin1 && c < 0x100) || (kind == Cp::Cp1252 && c >= 0xA0 && c < 0x100)) {
                out.push_back(static_cast<char>(c));
                continue;
            }
            bool found = false;
            if (kind == Cp::Cp1252)
                for (int k = 0; k < 32; ++k)
                    if (kCp1252High[k] == c) {
                        out.push_back(static_cast<char>(0x80 + k));
                        found = true;
                        break;
                    }
            if (!found) {
                out.push_back(def);
                if (used_default) *used_default = TRUE;
            }
        }
    }
    if (cb == 0) return static_cast<int>(out.size());
    if (out.size() > static_cast<size_t>(cb)) {
        gta6rt_last_error_set(kErrorInsufficientBuffer);
        return 0;
    }
    std::memcpy(dst, out.data(), out.size());
    return static_cast<int>(out.size());
}

int WINAPI w_lstrcmpW(const char16_t* a, const char16_t* b) {
    if (!a || !b) return a == b ? 0 : (a ? 1 : -1);
    for (;; ++a, ++b) {
        if (*a != *b) return *a < *b ? -1 : 1;
        if (!*a) return 0;
    }
}

// ---- locale (the host reports en-US; the title is not told another locale)

constexpr char16_t kLocaleName[] = u"en-US";

int copy_wide_result(const char16_t* text, char16_t* dst, int cch) {
    const int need = static_cast<int>(u16len(text)) + 1;
    if (cch == 0) return need;
    if (!dst || cch < need) {
        gta6rt_last_error_set(kErrorInsufficientBuffer);
        return 0;
    }
    std::memcpy(dst, text, need * sizeof(char16_t));
    return need;
}

int WINAPI w_GetUserDefaultLocaleName(char16_t* dst, int cch) { return copy_wide_result(kLocaleName, dst, cch); }
int WINAPI w_GetUserDefaultGeoName(char16_t* dst, int cch) { return copy_wide_result(u"US", dst, cch); }

DWORD WINAPI w_LocaleNameToLCID(const char16_t* name, DWORD flags) {
    (void)flags;
    if (!name) return 0x0409;
    if (u16ieq(name, u"en-US")) return 0x0409;
    if (u16ieq(name, u"en-GB")) return 0x0809;
    if (u16ieq(name, u"fr-FR")) return 0x040C;
    if (u16ieq(name, u"de-DE")) return 0x0407;
    if (u16ieq(name, u"es-ES")) return 0x0C0A;
    if (u16ieq(name, u"it-IT")) return 0x0410;
    if (u16ieq(name, u"ja-JP")) return 0x0411;
    if (u16ieq(name, u"ko-KR")) return 0x0412;
    if (u16ieq(name, u"pt-BR")) return 0x0416;
    if (u16ieq(name, u"ru-RU")) return 0x0419;
    if (u16ieq(name, u"zh-CN")) return 0x0804;
    if (u16ieq(name, u"zh-TW")) return 0x0404;
    gta6rt_last_error_set(kErrorInvalidParameter);
    return 0;
}

const char* error_text(DWORD id) {
    switch (id) {
    case 0: return "The operation completed successfully.";
    case 2: return "The system cannot find the file specified.";
    case 3: return "The system cannot find the path specified.";
    case 5: return "Access is denied.";
    case 6: return "The handle is invalid.";
    case 8: return "Not enough memory resources are available to process this command.";
    case 18: return "There are no more files.";
    case 32: return "The process cannot access the file because it is being used by another process.";
    case 50: return "The request is not supported.";
    case 80: return "The file exists.";
    case 87: return "The parameter is incorrect.";
    case 122: return "The data area passed to a system call is too small.";
    case 183: return "Cannot create a file when that file already exists.";
    case 1223: return "The operation was canceled by the user.";
    default: return nullptr;
    }
}

DWORD WINAPI w_FormatMessageA(DWORD flags, const void* source, DWORD id, DWORD lang, char* buf, DWORD size, void* args) {
    (void)source; (void)lang; (void)args;
    constexpr DWORD kAllocate = 0x100, kFromSystem = 0x1000, kFromString = 0x400, kIgnoreInserts = 0x200;
    std::string text;
    if (flags & kFromSystem) {
        const char* t = error_text(id);
        if (t) text = std::string(t) + "\r\n";
        else {
            gta6rt_last_error_set(1815);
            return 0;
        }
    } else if ((flags & kFromString) && (flags & kIgnoreInserts) && source) {
        text = static_cast<const char*>(source);
    } else {
        RTLOG(WARN, "FormatMessageA: flags 0x%x are not supported", flags);
        gta6rt_last_error_set(kErrorInvalidParameter);
        return 0;
    }
    if (flags & kAllocate) {
        char* out = static_cast<char*>(std::malloc(text.size() + 1));
        if (!out) return 0;
        std::memcpy(out, text.c_str(), text.size() + 1);
        *reinterpret_cast<char**>(buf) = out;
        return static_cast<DWORD>(text.size());
    }
    if (!buf || size < text.size() + 1) {
        gta6rt_last_error_set(kErrorInsufficientBuffer);
        return 0;
    }
    std::memcpy(buf, text.c_str(), text.size() + 1);
    return static_cast<DWORD>(text.size());
}

// ---- date and time pictures (en-US)

const char16_t* const kMonths[] = {u"January", u"February", u"March", u"April", u"May", u"June", u"July", u"August",
                                   u"September", u"October", u"November", u"December"};
const char16_t* const kDays[] = {u"Sunday", u"Monday", u"Tuesday", u"Wednesday", u"Thursday", u"Friday", u"Saturday"};

bool valid_systemtime(const SYSTEMTIME& s) {
    return s.wMonth >= 1 && s.wMonth <= 12 && s.wDay >= 1 && s.wDay <= 31 && s.wHour < 24 && s.wMinute < 60 &&
           s.wSecond < 60 && s.wDayOfWeek < 7;
}

std::u16string apply_picture(const char16_t* picture, const SYSTEMTIME& s) {
    std::u16string out;
    auto num = [&](unsigned v, int width) {
        std::string t = std::to_string(v);
        while (static_cast<int>(t.size()) < width) t.insert(t.begin(), '0');
        out += widen(t.c_str());
    };
    for (const char16_t* p = picture; *p;) {
        const char16_t c = *p;
        if (c == u'\'') {
            ++p;
            while (*p && !(*p == u'\'' && p[1] != u'\'')) {
                if (*p == u'\'' && p[1] == u'\'') ++p;
                out.push_back(*p++);
            }
            if (*p) ++p;
            continue;
        }
        int run = 1;
        while (p[run] == c) ++run;
        switch (c) {
        case u'd':
            if (run == 1) num(s.wDay, 1);
            else if (run == 2) num(s.wDay, 2);
            else if (run == 3) out += std::u16string(kDays[s.wDayOfWeek]).substr(0, 3);
            else out += kDays[s.wDayOfWeek];
            break;
        case u'M':
            if (run == 1) num(s.wMonth, 1);
            else if (run == 2) num(s.wMonth, 2);
            else if (run == 3) out += std::u16string(kMonths[s.wMonth - 1]).substr(0, 3);
            else out += kMonths[s.wMonth - 1];
            break;
        case u'y':
            if (run <= 2) num(s.wYear % 100, run);
            else num(s.wYear, run);
            break;
        case u'h': num((s.wHour % 12) ? (s.wHour % 12) : 12, run > 1 ? 2 : 1); break;
        case u'H': num(s.wHour, run > 1 ? 2 : 1); break;
        case u'm': num(s.wMinute, run > 1 ? 2 : 1); break;
        case u's': num(s.wSecond, run > 1 ? 2 : 1); break;
        case u't': out += (run == 1) ? (s.wHour < 12 ? u"A" : u"P") : (s.wHour < 12 ? u"AM" : u"PM"); break;
        default: for (int i = 0; i < run; ++i) out.push_back(c); break;
        }
        p += run;
    }
    return out;
}

int finish_format(const std::u16string& text, char16_t* dst, int cch) {
    const int need = static_cast<int>(text.size()) + 1;
    if (cch == 0) return need;
    if (!dst || cch < need) {
        gta6rt_last_error_set(kErrorInsufficientBuffer);
        return 0;
    }
    std::memcpy(dst, text.c_str(), need * sizeof(char16_t));
    return need;
}

int WINAPI w_GetDateFormatEx(const char16_t* locale, DWORD flags, const SYSTEMTIME* st, const char16_t* format,
                             char16_t* dst, int cch, const char16_t* calendar) {
    (void)locale; (void)calendar;
    SYSTEMTIME now;
    if (!st) {
        w_GetLocalTime(&now);
        st = &now;
    }
    if (!valid_systemtime(*st) || (cch < 0)) {
        gta6rt_last_error_set(kErrorInvalidParameter);
        return 0;
    }
    const char16_t* picture = format;
    if (!picture) picture = (flags & 0x2) ? u"dddd, MMMM d, yyyy" : u"M/d/yyyy";
    return finish_format(apply_picture(picture, *st), dst, cch);
}

int WINAPI w_GetTimeFormatEx(const char16_t* locale, DWORD flags, const SYSTEMTIME* st, const char16_t* format,
                             char16_t* dst, int cch) {
    (void)locale;
    SYSTEMTIME now;
    if (!st) {
        w_GetLocalTime(&now);
        st = &now;
    }
    if (!valid_systemtime(*st) || (cch < 0)) {
        gta6rt_last_error_set(kErrorInvalidParameter);
        return 0;
    }
    const char16_t* picture = format;
    if (!picture) picture = (flags & 0x8) ? u"HH:mm:ss" : u"h:mm:ss tt";
    return finish_format(apply_picture(picture, *st), dst, cch);
}

BOOL WINAPI w_GetDiskFreeSpaceExW(const char16_t* dir, ULARGE_INTEGER* avail, ULARGE_INTEGER* total, ULARGE_INTEGER* free_bytes) {
    std::string path = ".";
    if (dir && *dir) {
        fs::HostPath hp = fs::crt_write_path(narrow(dir) + "/.", true);
        if (hp.error) hp = fs::crt_read_path(narrow(dir));
        if (hp.error) {
            gta6rt_last_error_set(hp.error);
            return FALSE;
        }
        path = hp.path;
        while (path.size() > 1 && path.back() == '.') path.pop_back();
    }
    struct statfs sf;
    std::string probe = path;
    while (::statfs(probe.c_str(), &sf) != 0) {
        const size_t slash = probe.find_last_of('/');
        if (slash == std::string::npos || slash == 0) {
            gta6rt_last_error_set(kErrorPathNotFound);
            return FALSE;
        }
        probe.resize(slash);
    }
    if (avail) avail->QuadPart = static_cast<ULONGLONG>(sf.f_bavail) * sf.f_bsize;
    if (total) total->QuadPart = static_cast<ULONGLONG>(sf.f_blocks) * sf.f_bsize;
    if (free_bytes) free_bytes->QuadPart = static_cast<ULONGLONG>(sf.f_bfree) * sf.f_bsize;
    return TRUE;
}

}  // namespace

gta6rt_thread* thread_of_handle(void* h) {
    const auto v = reinterpret_cast<intptr_t>(h);
    if (v == -2) return gta6rt_thread_current();
    return static_cast<gta6rt_thread*>(gta6rt_handle_lookup(static_cast<uint64_t>(v), GTA6RT_HANDLE_THREAD));
}

uint64_t affinity_mask() {
    const uint32_t n = logical_cpus() > 64 ? 64 : logical_cpus();
    return n >= 64 ? ~0ull : ((1ull << n) - 1);
}

uint64_t filetime_from_unix(int64_t seconds, int64_t nanos) {
    return static_cast<uint64_t>(seconds + static_cast<int64_t>(kEpochDiff)) * kTicksPerSecond + static_cast<uint64_t>(nanos / 100);
}

}  // namespace rt::k32

namespace {
using namespace rt::k32;
#define K(n) SHIM("kernel32.dll", #n, w_##n)
}  // namespace

SHIM_TABLE(k32_misc,
    K(GetLastError), K(SetLastError), K(IsDebuggerPresent), K(OutputDebugStringA), K(GetCurrentProcess),
    K(GetCurrentProcessId), K(GetCurrentThreadId), K(GetProcessId), K(TerminateProcess), K(GetStartupInfoW),
    K(IsProcessorFeaturePresent), K(GetCurrentThreadStackLimits), K(SetThreadPriority), K(GetThreadPriority),
    K(SetProcessPriorityBoost), K(GetThreadGroupAffinity), K(RegisterAppConstrainedChangeNotification),
    K(UnregisterAppConstrainedChangeNotification), K(WerRegisterFile), K(SetNamedPipeHandleState),
    K(WaitNamedPipeW), K(LocalAlloc), K(LocalFree), K(InitializeSListHead), K(GlobalMemoryStatusEx),
    K(GetLogicalProcessorInformation), K(FileTimeToSystemTime), K(SystemTimeToFileTime), K(GetSystemTimeAsFileTime),
    SHIM("kernel32.dll", "GetSystemTimePreciseAsFileTime", w_GetSystemTimeAsFileTime), K(GetSystemTime),
    K(GetLocalTime), K(FileTimeToLocalFileTime), K(LocalFileTimeToFileTime), K(GetTickCount64), K(Sleep),
    K(QueryPerformanceCounter), K(QueryPerformanceFrequency), K(MultiByteToWideChar), K(WideCharToMultiByte),
    K(lstrcmpW), K(GetUserDefaultLocaleName), K(GetUserDefaultGeoName), K(LocaleNameToLCID), K(FormatMessageA),
    K(GetDateFormatEx), K(GetTimeFormatEx), K(GetDiskFreeSpaceExW),
);
