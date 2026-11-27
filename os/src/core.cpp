#include "internal.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mach/mach_time.h>
#include <unistd.h>

using namespace gta6rt_os;

namespace {

thread_local uint32_t t_last_error;

int g_log_level = -1;
pthread_mutex_t g_log_mx = PTHREAD_MUTEX_INITIALIZER;

int log_level() {
    int l = __atomic_load_n(&g_log_level, __ATOMIC_RELAXED);
    if (l >= 0) return l;
    l = GTA6RT_LOG_INFO;
    if (const char* e = getenv("GTA6RT_LOG_LEVEL")) {
        if (!strcmp(e, "error")) l = GTA6RT_LOG_ERROR;
        else if (!strcmp(e, "warn")) l = GTA6RT_LOG_WARN;
        else if (!strcmp(e, "info")) l = GTA6RT_LOG_INFO;
        else if (!strcmp(e, "debug")) l = GTA6RT_LOG_DEBUG;
        else if (!strcmp(e, "trace")) l = GTA6RT_LOG_TRACE;
        else if (*e >= '0' && *e <= '4') l = *e - '0';
    }
    __atomic_store_n(&g_log_level, l, __ATOMIC_RELAXED);
    return l;
}

uint64_t default_invoke(gta6rt_gva fn, const uint64_t* a, uint32_t n, void*) {
    typedef uint64_t(GTA6RT_GUEST_ABI * F6)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    uint64_t v[6] = {0, 0, 0, 0, 0, 0};
    for (uint32_t i = 0; i < n && i < 6; i++) v[i] = a[i];
    return ((F6)(uintptr_t)fn)(v[0], v[1], v[2], v[3], v[4], v[5]);
}

std::atomic<gta6rt_os_invoke_fn> g_invoke{default_invoke};
std::atomic<void*> g_invoke_user{nullptr};

mach_timebase_info_data_t timebase() {
    static mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t;
        mach_timebase_info(&t);
        return t;
    }();
    return tb;
}

} // namespace

namespace gta6rt_os {

void set_error(uint32_t code) { t_last_error = code; }

uint32_t errno_to_win32(int e) {
    switch (e) {
    case 0: return ERROR_SUCCESS;
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES:
    case EPERM:
    case EISDIR:
    case EROFS: return ERROR_ACCESS_DENIED;
    case EEXIST: return ERROR_FILE_EXISTS;
    case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
    case EMFILE:
    case ENFILE: return ERROR_TOO_MANY_OPEN_FILES;
    case ENOSPC: return ERROR_DISK_FULL;
    case ENAMETOOLONG: return ERROR_FILENAME_EXCED_RANGE;
    case EBADF: return ERROR_INVALID_HANDLE;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case EINVAL: return ERROR_INVALID_PARAMETER;
    case EBUSY: return ERROR_BUSY;
    case EXDEV: return ERROR_NOT_SAME_DEVICE;
    case EPIPE: return ERROR_BROKEN_PIPE;
    case ENOTSUP: return ERROR_NOT_SUPPORTED;
    case ETIMEDOUT: return ERROR_TIMEOUT;
    case EIO: return ERROR_GEN_FAILURE;
    case ELOOP: return ERROR_CANTOPEN;
    case EFAULT: return ERROR_NOACCESS;
    default: return ERROR_GEN_FAILURE;
    }
}

gta6rt_status errno_to_status(int e) {
    switch (e) {
    case 0: return GTA6RT_OK;
    case ENOENT:
    case ENOTDIR: return GTA6RT_E_NOT_FOUND;
    case EACCES:
    case EPERM:
    case EROFS:
    case EISDIR: return GTA6RT_E_ACCESS;
    case EEXIST: return GTA6RT_E_ALREADY_EXISTS;
    case ENOMEM: return GTA6RT_E_NO_MEMORY;
    case EINVAL:
    case EBADF: return GTA6RT_E_INVALID_ARG;
    case EBUSY: return GTA6RT_E_BUSY;
    case ENOTSUP: return GTA6RT_E_UNSUPPORTED;
    case ETIMEDOUT: return GTA6RT_E_TIMEOUT;
    default: return GTA6RT_E_IO;
    }
}

uint32_t status_to_win32(gta6rt_status s) {
    switch (s) {
    case GTA6RT_OK: return ERROR_SUCCESS;
    case GTA6RT_E_NOT_IMPLEMENTED: return ERROR_CALL_NOT_IMPLEMENTED;
    case GTA6RT_E_INVALID_ARG: return ERROR_INVALID_PARAMETER;
    case GTA6RT_E_NO_MEMORY: return ERROR_NOT_ENOUGH_MEMORY;
    case GTA6RT_E_NOT_FOUND: return ERROR_NOT_FOUND;
    case GTA6RT_E_ALREADY_EXISTS: return ERROR_ALREADY_EXISTS;
    case GTA6RT_E_UNSUPPORTED: return ERROR_NOT_SUPPORTED;
    case GTA6RT_E_TIMEOUT: return ERROR_TIMEOUT;
    case GTA6RT_E_IO: return ERROR_GEN_FAILURE;
    case GTA6RT_E_ADDRESS_IN_USE: return ERROR_INVALID_ADDRESS;
    case GTA6RT_E_ACCESS: return ERROR_ACCESS_DENIED;
    case GTA6RT_E_BUSY: return ERROR_BUSY;
    default: return ERROR_GEN_FAILURE;
    }
}

bool log_enabled(gta6rt_log_level lvl) { return (int)lvl <= log_level(); }

size_t u16_len(const char16_t* s) {
    size_t n = 0;
    if (s)
        while (s[n]) n++;
    return n;
}

std::string u16_to_utf8(const char16_t* s, size_t n) {
    std::string out;
    if (!s) return out;
    if (n == (size_t)-1) n = u16_len(s);
    out.reserve(n);
    for (size_t i = 0; i < n; i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c < 0x80) {
            out.push_back((char)c);
        } else if (c < 0x800) {
            out.push_back((char)(0xC0 | (c >> 6)));
            out.push_back((char)(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back((char)(0xE0 | (c >> 12)));
            out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (c & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (c >> 18)));
            out.push_back((char)(0x80 | ((c >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

std::u16string utf8_to_u16(const char* s, size_t n) {
    std::u16string out;
    if (!s) return out;
    if (n == (size_t)-1) n = strlen(s);
    const unsigned char* p = (const unsigned char*)s;
    size_t i = 0;
    while (i < n) {
        uint32_t c = p[i];
        int len = 1;
        uint32_t min = 0;
        if (c < 0x80) {
        } else if ((c & 0xE0) == 0xC0) {
            len = 2, c &= 0x1F, min = 0x80;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3, c &= 0x0F, min = 0x800;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4, c &= 0x07, min = 0x10000;
        } else {
            out.push_back(0xFFFD);
            i++;
            continue;
        }
        if (i + len > n) {
            out.push_back(0xFFFD);
            break;
        }
        bool bad = false;
        for (int k = 1; k < len; k++) {
            if ((p[i + k] & 0xC0) != 0x80) {
                bad = true;
                break;
            }
            c = (c << 6) | (p[i + k] & 0x3F);
        }
        if (bad || c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
            out.push_back(0xFFFD);
            i++;
            continue;
        }
        i += len;
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back((char16_t)(0xD800 + (c >> 10)));
            out.push_back((char16_t)(0xDC00 + (c & 0x3FF)));
        } else {
            out.push_back((char16_t)c);
        }
    }
    return out;
}

DWORD copy_out_w(const std::u16string& src, LPWSTR dst, DWORD cap) {
    if (!dst || cap < src.size() + 1) return (DWORD)src.size() + 1;
    memcpy(dst, src.data(), src.size() * 2);
    dst[src.size()] = 0;
    return (DWORD)src.size();
}

DWORD copy_out_a(const std::string& src, LPSTR dst, DWORD cap) {
    if (!dst || cap < src.size() + 1) return (DWORD)src.size() + 1;
    memcpy(dst, src.data(), src.size());
    dst[src.size()] = 0;
    return (DWORD)src.size();
}

uint64_t monotonic_ns() {
    mach_timebase_info_data_t tb = timebase();
    uint64_t t = mach_absolute_time();
    if (tb.numer == tb.denom) return t;
    return (uint64_t)((unsigned __int128)t * tb.numer / tb.denom);
}

uint64_t deadline_from_timeout(uint64_t timeout_ns) {
    if (timeout_ns == GTA6RT_WAIT_INFINITE) return UINT64_MAX;
    uint64_t now = monotonic_ns();
    return timeout_ns > UINT64_MAX - now - 1 ? UINT64_MAX - 1 : now + timeout_ns;
}

uint64_t ms_to_ns(DWORD ms) { return ms == INFINITE ? GTA6RT_WAIT_INFINITE : (uint64_t)ms * 1000000ull; }

uint64_t filetime_now() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 10000000ull + (uint64_t)ts.tv_nsec / 100 + 116444736000000000ull;
}

} // namespace gta6rt_os

extern "C" {

const char* gta6rt_status_name(gta6rt_status s) {
    switch (s) {
    case GTA6RT_OK: return "ok";
    case GTA6RT_E_NOT_IMPLEMENTED: return "not_implemented";
    case GTA6RT_E_INVALID_ARG: return "invalid_arg";
    case GTA6RT_E_NO_MEMORY: return "no_memory";
    case GTA6RT_E_NOT_FOUND: return "not_found";
    case GTA6RT_E_ALREADY_EXISTS: return "already_exists";
    case GTA6RT_E_UNSUPPORTED: return "unsupported";
    case GTA6RT_E_TIMEOUT: return "timeout";
    case GTA6RT_E_IO: return "io";
    case GTA6RT_E_BAD_FORMAT: return "bad_format";
    case GTA6RT_E_ADDRESS_IN_USE: return "address_in_use";
    case GTA6RT_E_ACCESS: return "access";
    case GTA6RT_E_BUSY: return "busy";
    case GTA6RT_E_ABANDONED: return "abandoned";
    default: return "unknown";
    }
}

uint32_t gta6rt_last_error_get(void) { return t_last_error; }
void gta6rt_last_error_set(uint32_t code) { t_last_error = code; }

void gta6rt_log(gta6rt_log_level level, const char* module, const char* fmt, ...) {
    if ((int)level > log_level()) return;
    static const char* names[] = {"E", "W", "I", "D", "T"};
    char buf[2048];
    int off = snprintf(buf, sizeof buf, "[%s %s] ", names[level < 5 ? level : 4], module ? module : "?");
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, sizeof buf - off - 1, fmt, ap);
    va_end(ap);
    size_t len = (size_t)off + (n < 0 ? 0 : (size_t)n);
    if (len > sizeof buf - 2) len = sizeof buf - 2;
    if (len == 0 || buf[len - 1] != '\n') buf[len++] = '\n';
    pthread_mutex_lock(&g_log_mx);
    (void)!write(2, buf, len);
    pthread_mutex_unlock(&g_log_mx);
}

void gta6rt_log_signal_safe(const char* text) {
    if (!text) return;
    size_t n = 0;
    while (text[n]) n++;
    (void)!write(2, text, n);
}

gta6rt_status gta6rt_os_set_invoker(gta6rt_os_invoke_fn fn, void* user) {
    g_invoke_user.store(user);
    g_invoke.store(fn ? fn : default_invoke);
    return GTA6RT_OK;
}

uint64_t gta6rt_os_invoke(gta6rt_gva fn, const uint64_t* args, uint32_t count) {
    return g_invoke.load()(fn, args, count, g_invoke_user.load());
}

DWORD WINAPI GetLastError(void) { return t_last_error; }
void WINAPI SetLastError(DWORD dwErrCode) { t_last_error = dwErrCode; }

} // extern "C"
