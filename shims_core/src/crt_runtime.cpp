// UCRT runtime imports (api-ms-win-crt-runtime, filesystem): errno, startup helpers, onexit tables.
#include <sys/stat.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include <gta6rt/isa.h>

#include "crt_hooks.h"
#include "file_system.h"
#include "process_shim.h"
#include "shims.h"
#include "util.h"

namespace rt::crt {
namespace {

int* WINAPI w_errno() { return &errno; }
int WINAPI w_get_errno(int* out) {
    if (!out) return 22;
    *out = errno;
    return 0;
}
int WINAPI w_set_errno(int value) {
    errno = value;
    return 0;
}

int WINAPI w_initialize_narrow_environment() { return 0; }

int g_app_type = 0;
void WINAPI w_set_app_type(int type) { g_app_type = type; }

uint64_t g_invalid_parameter_handler = 0;
uint64_t WINAPI w_set_invalid_parameter_handler(uint64_t handler) {
    return __atomic_exchange_n(&g_invalid_parameter_handler, handler, __ATOMIC_SEQ_CST);
}

void invoke_invalid_parameter() {
    const uint64_t handler = __atomic_load_n(&g_invalid_parameter_handler, __ATOMIC_SEQ_CST);
    if (!handler) {
        RTLOG(ERROR, "invalid parameter passed to a CRT function and no handler is set; ending the run");
        process::abort_run(3);
    }
    gta6rt_guest_args a{};
    a.count = 5;
    gta6rt_guest_ret r{};
    gta6rt_isa_call_guest(handler, &a, &r);
}

void WINAPI w_invalid_parameter_noinfo() { invoke_invalid_parameter(); }
void WINAPI w_invalid_parameter_noinfo_noreturn() {
    invoke_invalid_parameter();
    process::abort_run(3);
}

// _initterm: pointers to guest `void(*)(void)`; _initterm_e: `int(*)(void)`, stops at the first non-zero result.
void WINAPI w_initterm(const uint64_t* first, const uint64_t* last) {
    for (const uint64_t* p = first; p && p < last; ++p)
        if (*p) gta6rt_isa_call_guest4(*p, 0, 0, 0, 0);
}
int WINAPI w_initterm_e(const uint64_t* first, const uint64_t* last) {
    for (const uint64_t* p = first; p && p < last; ++p) {
        if (!*p) continue;
        const int r = static_cast<int32_t>(gta6rt_isa_call_guest4(*p, 0, 0, 0, 0));
        if (r != 0) return r;
    }
    return 0;
}

// _onexit_table_t is three pointers {first, last, end} to an array of guest function pointers. The array is host
// memory; the guest never reads it.
struct OnexitTable {
    uint64_t first, last, end;
};
std::mutex g_onexit_mutex;

int WINAPI w_initialize_onexit_table(OnexitTable* table) {
    if (!table) return -1;
    std::lock_guard lock(g_onexit_mutex);
    table->first = table->last = table->end = 0;
    return 0;
}

int WINAPI w_register_onexit_function(OnexitTable* table, uint64_t fn) {
    if (!table) return -1;
    std::lock_guard lock(g_onexit_mutex);
    auto* first = reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(table->first));
    auto* last = reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(table->last));
    auto* end = reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(table->end));
    if (last == end) {
        const size_t used = static_cast<size_t>(last - first);
        const size_t cap = used ? used * 2 : 32;
        auto* grown = static_cast<uint64_t*>(std::realloc(first, cap * sizeof(uint64_t)));
        if (!grown) return -1;
        first = grown;
        last = grown + used;
        end = grown + cap;
    }
    *last++ = fn;
    table->first = reinterpret_cast<uintptr_t>(first);
    table->last = reinterpret_cast<uintptr_t>(last);
    table->end = reinterpret_cast<uintptr_t>(end);
    return 0;
}

int WINAPI w_execute_onexit_table(OnexitTable* table) {
    if (!table) return -1;
    std::vector<uint64_t> fns;
    {
        std::lock_guard lock(g_onexit_mutex);
        auto* first = reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(table->first));
        auto* last = reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(table->last));
        for (auto* p = last; p > first;) fns.push_back(*--p);
        std::free(first);
        table->first = table->last = table->end = 0;
    }
    for (uint64_t fn : fns)
        if (fn) gta6rt_isa_call_guest4(fn, 0, 0, 0, 0);
    return 0;
}

uint64_t g_thread_exit_callback = 0;
int WINAPI w_register_thread_local_exe_atexit_callback(uint64_t cb) {
    __atomic_store_n(&g_thread_exit_callback, cb, __ATOMIC_SEQ_CST);
    RTLOG(INFO, "thread-local exe atexit callback registered at 0x%llx", static_cast<unsigned long long>(cb));
    return 0;
}

int WINAPI w_remove(const char* path) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }
    uint32_t error = 0;
    if (fs::crt_remove(path, error)) return 0;
    errno = (error == 2 || error == 3) ? ENOENT : EACCES;
    return -1;
}

// Windows `struct _stat64` (56 bytes).
struct WinStat64 {
    uint32_t st_dev;
    uint16_t st_ino;
    uint16_t st_mode;
    int16_t st_nlink;
    int16_t st_uid;
    int16_t st_gid;
    uint32_t st_rdev;
    int64_t st_size;
    int64_t t_atime;
    int64_t t_mtime;
    int64_t t_ctime;
};
static_assert(sizeof(WinStat64) == 56 && offsetof(WinStat64, st_rdev) == 16 && offsetof(WinStat64, st_size) == 24);

int WINAPI w_wstat64(const char16_t* path, WinStat64* out) {
    if (!path || !out) {
        errno = EINVAL;
        return -1;
    }
    const fs::HostPath hp = fs::crt_read_path(narrow(path));
    if (hp.error) {
        errno = ENOENT;
        return -1;
    }
    struct stat st;
    if (::stat(hp.path.c_str(), &st) != 0) return -1;
    std::memset(out, 0, sizeof *out);
    out->st_dev = static_cast<uint32_t>(st.st_dev);
    out->st_mode = static_cast<uint16_t>((S_ISDIR(st.st_mode) ? 0x4000 : 0x8000) | ((st.st_mode & S_IRUSR) ? 0x100 : 0) |
                                          ((st.st_mode & S_IWUSR) ? 0x80 : 0) | ((st.st_mode & S_IXUSR) ? 0x40 : 0));
    out->st_nlink = 1;
    out->st_size = st.st_size;
    out->t_atime = st.st_atimespec.tv_sec;
    out->t_mtime = st.st_mtimespec.tv_sec;
    out->t_ctime = st.st_ctimespec.tv_sec;
    return 0;
}

}  // namespace

uint64_t thread_exit_callback() { return __atomic_load_n(&g_thread_exit_callback, __ATOMIC_SEQ_CST); }

}  // namespace rt::crt

namespace {
using namespace rt::crt;
}  // namespace

SHIM_TABLE(crt_runtime,
    SHIM("ucrtbase.dll", "_errno", w_errno), SHIM("ucrtbase.dll", "_get_errno", w_get_errno),
    SHIM("ucrtbase.dll", "_set_errno", w_set_errno),
    SHIM("ucrtbase.dll", "_initialize_narrow_environment", w_initialize_narrow_environment),
    SHIM("ucrtbase.dll", "_set_app_type", w_set_app_type),
    SHIM("ucrtbase.dll", "_set_invalid_parameter_handler", w_set_invalid_parameter_handler),
    SHIM("ucrtbase.dll", "_invalid_parameter_noinfo", w_invalid_parameter_noinfo),
    SHIM("ucrtbase.dll", "_invalid_parameter_noinfo_noreturn", w_invalid_parameter_noinfo_noreturn),
    SHIM("ucrtbase.dll", "_initterm", w_initterm), SHIM("ucrtbase.dll", "_initterm_e", w_initterm_e),
    SHIM("ucrtbase.dll", "_initialize_onexit_table", w_initialize_onexit_table),
    SHIM("ucrtbase.dll", "_register_onexit_function", w_register_onexit_function),
    SHIM("ucrtbase.dll", "_execute_onexit_table", w_execute_onexit_table),
    SHIM("ucrtbase.dll", "_register_thread_local_exe_atexit_callback", w_register_thread_local_exe_atexit_callback),
    SHIM("ucrtbase.dll", "remove", w_remove), SHIM("ucrtbase.dll", "_wstat64", w_wstat64),
);
