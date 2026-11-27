// The kernel32/user32 subset the ported leonida sources of this module call (win32_host.h). os/ objects first (its
// handle table, events, VM), private host objects when os/ answers GTA6RT_E_NOT_IMPLEMENTED.

#include "win32_host.h"
#include "mac_window.h"
#include "fault_guard.h"

#include <gta6rt/os.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

thread_local DWORD t_last_error = 0;

void set_error(DWORD e) { t_last_error = e; }

// ---- private handle objects ----

constexpr std::uint32_t kMagic = 0x6b766472;  // "rdvk"

enum class Kind { event, semaphore, thread, timer, file, mapping, heap, wait };

struct Object {
    explicit Object(Kind k) : kind(k) {}
    virtual ~Object() = default;
    std::uint32_t magic = kMagic;
    Kind kind;
    std::atomic<int> refs{1};
    void retain() { refs.fetch_add(1, std::memory_order_relaxed); }
    void release() {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
    }
};

// Anything that WaitForSingleObject can wait on.
struct Waitable : Object {
    using Object::Object;
    std::mutex mutex;
    std::condition_variable cv;
    // Called with `mutex` held: is it signalled, and if so consume the signal (auto-reset objects).
    virtual bool try_acquire() = 0;
    // Steady-clock time at which the object signals by itself (timers), or max.
    virtual std::chrono::steady_clock::time_point deadline() { return std::chrono::steady_clock::time_point::max(); }
};

struct Event final : Waitable {
    Event(bool manual, bool initial) : Waitable(Kind::event), manual(manual), signalled(initial) {}
    bool manual;
    bool signalled;
    bool try_acquire() override {
        if (!signalled) return false;
        if (!manual) signalled = false;
        return true;
    }
};

struct Semaphore final : Waitable {
    Semaphore(LONG initial, LONG maximum) : Waitable(Kind::semaphore), count(initial), maximum(maximum) {}
    LONG count, maximum;
    bool try_acquire() override {
        if (count <= 0) return false;
        --count;
        return true;
    }
};

struct Thread final : Waitable {
    Thread() : Waitable(Kind::thread) {}
    bool done = false;
    DWORD exit_code = STILL_ACTIVE;
    pthread_t pthread{};
    bool try_acquire() override { return done; }
};

struct Timer final : Waitable {
    explicit Timer(bool manual) : Waitable(Kind::timer), manual(manual) {}
    bool manual;
    bool armed = false;
    bool signalled = false;
    std::chrono::steady_clock::time_point due{};
    bool try_acquire() override {
        if (armed && std::chrono::steady_clock::now() >= due) {
            armed = false;
            signalled = true;
        }
        if (!signalled) return false;
        if (!manual) signalled = false;
        return true;
    }
    std::chrono::steady_clock::time_point deadline() override {
        return armed ? due : std::chrono::steady_clock::time_point::max();
    }
};

struct File final : Object {
    File() : Object(Kind::file) {}
    ~File() override {
        if (fd >= 0 && owns) ::close(fd);
        if (delete_on_close && !path.empty()) ::unlink(path.c_str());
    }
    int fd = -1;
    bool owns = true;
    bool delete_on_close = false;
    std::string path;
};

struct Mapping final : Object {
    Mapping() : Object(Kind::mapping) {}
    ~Mapping() override {
        if (fd >= 0) ::close(fd);
    }
    int fd = -1;
    bool writable = false;
    std::uint64_t size = 0;
};

struct Heap final : Object {
    Heap() : Object(Kind::heap) {}
};

HANDLE to_handle(Object* o) {
    return reinterpret_cast<HANDLE>(reinterpret_cast<std::uintptr_t>(o) | kRthostPrivateHandleTag);
}

bool is_private(HANDLE h) {
    const auto v = reinterpret_cast<std::uintptr_t>(h);
    return (v & kRthostPrivateHandleTag) && v != reinterpret_cast<std::uintptr_t>(INVALID_HANDLE_VALUE) &&
           v < (kRthostPrivateHandleTag << 1);
}

Object* object_of(HANDLE h) {
    if (!is_private(h)) return nullptr;
    auto* o = reinterpret_cast<Object*>(reinterpret_cast<std::uintptr_t>(h) & ~kRthostPrivateHandleTag);
    return o->magic == kMagic ? o : nullptr;
}

template <class T>
T* object_as(HANDLE h, Kind kind) {
    Object* o = object_of(h);
    return o && o->kind == kind ? static_cast<T*>(o) : nullptr;
}

Waitable* waitable_of(HANDLE h) {
    Object* o = object_of(h);
    if (!o) return nullptr;
    switch (o->kind) {
    case Kind::event:
    case Kind::semaphore:
    case Kind::thread:
    case Kind::timer: return static_cast<Waitable*>(o);
    default: return nullptr;
    }
}

const HANDLE kCurrentProcess = reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-1));
const HANDLE kCurrentThread = reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-2));

std::uint64_t ms_to_ns(DWORD ms) { return ms == INFINITE ? GTA6RT_WAIT_INFINITE : std::uint64_t(ms) * 1000000ull; }

DWORD wait_private(Waitable* w, DWORD ms) {
    using clock = std::chrono::steady_clock;
    const auto until = ms == INFINITE ? clock::time_point::max() : clock::now() + std::chrono::milliseconds(ms);
    std::unique_lock lock(w->mutex);
    for (;;) {
        if (w->try_acquire()) return WAIT_OBJECT_0;
        const auto now = clock::now();
        if (now >= until) return WAIT_TIMEOUT;
        const auto wake = std::min(until, w->deadline());
        if (wake == clock::time_point::max()) w->cv.wait(lock);
        else w->cv.wait_until(lock, wake);
    }
}

gta6rt_waitable* os_waitable(HANDLE h) {
    const auto v = reinterpret_cast<std::uint64_t>(h);
    if (auto* w = static_cast<gta6rt_waitable*>(gta6rt_handle_lookup(v, GTA6RT_HANDLE_WAITABLE))) return w;
    if (auto* t = static_cast<gta6rt_thread*>(gta6rt_handle_lookup(v, GTA6RT_HANDLE_THREAD)))
        return gta6rt_thread_waitable(t);
    return nullptr;
}

HANDLE os_event(bool manual, bool initial) {
    gta6rt_waitable* w = nullptr;
    if (gta6rt_event_create(manual, initial, &w) != GTA6RT_OK || !w) return nullptr;
    std::uint64_t h = 0;
    if (gta6rt_handle_alloc(GTA6RT_HANDLE_WAITABLE, w, [](void* p) { gta6rt_waitable_release(static_cast<gta6rt_waitable*>(p)); },
                            &h) != GTA6RT_OK) {
        gta6rt_waitable_release(w);
        return nullptr;
    }
    return reinterpret_cast<HANDLE>(h);
}

DWORD prot_to_win(std::uint32_t prot) {
    const bool r = prot & GTA6RT_PROT_READ, w = prot & GTA6RT_PROT_WRITE, x = prot & GTA6RT_PROT_EXEC;
    DWORD out = !r && !w && !x ? PAGE_NOACCESS
                : x            ? (w ? PAGE_EXECUTE_READWRITE : r ? PAGE_EXECUTE_READ : PAGE_EXECUTE)
                               : (w ? PAGE_READWRITE : PAGE_READONLY);
    if (prot & GTA6RT_PROT_GUARD) out |= PAGE_GUARD;
    return out;
}

std::uint32_t win_to_prot(DWORD p) {
    std::uint32_t out = GTA6RT_PROT_NONE;
    switch (p & 0xff) {
    case PAGE_READONLY: out = GTA6RT_PROT_READ; break;
    case PAGE_READWRITE:
    case PAGE_WRITECOPY: out = GTA6RT_PROT_READ | GTA6RT_PROT_WRITE; break;
    case PAGE_EXECUTE: out = GTA6RT_PROT_EXEC; break;
    case PAGE_EXECUTE_READ: out = GTA6RT_PROT_READ | GTA6RT_PROT_EXEC; break;
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY: out = GTA6RT_PROT_READ | GTA6RT_PROT_WRITE | GTA6RT_PROT_EXEC; break;
    default: break;
    }
    if (p & PAGE_GUARD) out |= GTA6RT_PROT_GUARD;
    return out;
}

int posix_prot(std::uint32_t prot) {
    return (prot & GTA6RT_PROT_READ ? PROT_READ : 0) | (prot & GTA6RT_PROT_WRITE ? PROT_WRITE : 0) |
           (prot & GTA6RT_PROT_EXEC ? PROT_EXEC : 0);
}

// Host fallback region table for VirtualAlloc/VirtualFree and MapViewOfFile/UnmapViewOfFile.
std::mutex g_regions_mutex;
std::map<std::uintptr_t, std::size_t> g_reservations;
std::map<std::uintptr_t, std::size_t> g_views;

// mach_vm_region from `address`: the region holding it or the next one above.
bool mach_region(std::uintptr_t address, std::uintptr_t& start, std::uintptr_t& size, vm_prot_t& prot) {
    mach_vm_address_t a = address;
    mach_vm_size_t s = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &a, &s, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info),
                       &count, &object) != KERN_SUCCESS)
        return false;
    start = std::uintptr_t(a);
    size = std::uintptr_t(s);
    prot = info.protection;
    return true;
}

bool range_has(const void* p, std::size_t bytes, bool write) {
    if (!bytes) return true;
    auto at = reinterpret_cast<std::uintptr_t>(p);
    const std::uintptr_t end = at + bytes;
    if (end < at) return false;
    gta6rt_vm_region region{};
    const gta6rt_status os = gta6rt_vm_query(p, &region);
    if (os != GTA6RT_E_NOT_IMPLEMENTED) {
        while (at < end) {
            if (gta6rt_vm_query(reinterpret_cast<const void*>(at), &region) != GTA6RT_OK) return false;
            if (region.state != GTA6RT_VM_STATE_COMMITTED || (region.prot & GTA6RT_PROT_GUARD) ||
                !(region.prot & (write ? GTA6RT_PROT_WRITE : GTA6RT_PROT_READ)))
                return false;
            const auto next = reinterpret_cast<std::uintptr_t>(region.base) + region.size;
            if (next <= at) return false;
            at = next;
        }
        return true;
    }
    while (at < end) {
        std::uintptr_t start = 0, size = 0;
        vm_prot_t prot = 0;
        if (!mach_region(at, start, size, prot) || start > at) return false;
        if (!(prot & (write ? VM_PROT_WRITE : VM_PROT_READ))) return false;
        at = start + size;
    }
    return true;
}

std::string executable_path() {
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) return {};
    path.resize(std::strlen(path.c_str()));
    char resolved[PATH_MAX];
    return realpath(path.c_str(), resolved) ? std::string(resolved) : path;
}

template <class Char>
std::size_t length_of(const Char* s) {
    std::size_t n = 0;
    while (s[n]) ++n;
    return n;
}

void append_utf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xc0 | (c >> 6));
        out += char(0x80 | (c & 0x3f));
    } else if (c < 0x10000) {
        out += char(0xe0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3f));
        out += char(0x80 | (c & 0x3f));
    } else {
        out += char(0xf0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3f));
        out += char(0x80 | ((c >> 6) & 0x3f));
        out += char(0x80 | (c & 0x3f));
    }
}

std::u32string decode_utf8(const char* s, std::size_t n) {
    std::u32string out;
    for (std::size_t i = 0; i < n;) {
        const auto b = static_cast<unsigned char>(s[i]);
        char32_t c = 0xfffd;
        std::size_t len = 1;
        if (b < 0x80) c = b;
        else if ((b & 0xe0) == 0xc0 && i + 1 < n) c = ((b & 0x1f) << 6) | (s[i + 1] & 0x3f), len = 2;
        else if ((b & 0xf0) == 0xe0 && i + 2 < n)
            c = ((b & 0x0f) << 12) | ((s[i + 1] & 0x3f) << 6) | (s[i + 2] & 0x3f), len = 3;
        else if ((b & 0xf8) == 0xf0 && i + 3 < n)
            c = ((b & 0x07) << 18) | ((s[i + 1] & 0x3f) << 12) | ((s[i + 2] & 0x3f) << 6) | (s[i + 3] & 0x3f), len = 4;
        out += c;
        i += len;
    }
    return out;
}

std::u16string to_utf16(const std::u32string& s) {
    std::u16string out;
    for (char32_t c : s) {
        if (c >= 0x10000) {
            c -= 0x10000;
            out += char16_t(0xd800 | (c >> 10));
            out += char16_t(0xdc00 | (c & 0x3ff));
        } else {
            out += char16_t(c);
        }
    }
    return out;
}

std::string utf8_of(const rthost_wstr& s, int count) {
    if (s.a) return count < 0 ? std::string(s.a) : std::string(s.a, std::size_t(count));
    if (s.w) return rthost::to_utf8(s.w, count);
    if (s.u) return rthost::to_utf8(s.u, count);
    return {};
}

// Writes `text` (no terminator in it) into a wide buffer of `size` chars, Win32 GetEnvironmentVariableW style:
// the length without the terminator on success, the needed size with it when too small.
DWORD put_wide(const std::string& text, rthost_wbuf out, DWORD size) {
    const std::u32string wide = decode_utf8(text.data(), text.size());
    if (out.w) {
        if (wide.size() + 1 > size) return DWORD(wide.size() + 1);
        for (std::size_t i = 0; i < wide.size(); ++i) out.w[i] = wchar_t(wide[i]);
        out.w[wide.size()] = 0;
        return DWORD(wide.size());
    }
    const std::u16string u16 = to_utf16(wide);
    if (!out.u || u16.size() + 1 > size) return DWORD(u16.size() + 1);
    std::memcpy(out.u, u16.data(), u16.size() * 2);
    out.u[u16.size()] = 0;
    return DWORD(u16.size());
}

DWORD errno_to_win(int e) {
    switch (e) {
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES:
    case EPERM: return ERROR_ACCESS_DENIED;
    case EEXIST: return ERROR_FILE_EXISTS;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case ENOSPC: return ERROR_DISK_FULL;
    default: return ERROR_GEN_FAILURE;
    }
}

HANDLE open_file(const std::string& raw, DWORD access, DWORD disposition, DWORD flags) {
    const std::string path = rthost::host_path(raw);
    const bool read = access & (GENERIC_READ | GENERIC_ALL), write = access & (GENERIC_WRITE | GENERIC_ALL);
    int oflags = O_CLOEXEC | (read && write ? O_RDWR : write ? O_WRONLY : O_RDONLY);
    switch (disposition) {
    case CREATE_NEW: oflags |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: oflags |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS: oflags |= O_CREAT; break;
    case TRUNCATE_EXISTING: oflags |= O_TRUNC; break;
    default: break;
    }
    struct stat st{};
    const bool existed = ::stat(path.c_str(), &st) == 0;
    const int fd = ::open(path.c_str(), oflags, 0644);
    if (fd < 0) {
        set_error(errno == EEXIST ? ERROR_FILE_EXISTS : errno_to_win(errno));
        return INVALID_HANDLE_VALUE;
    }
    auto* f = new File;
    f->fd = fd;
    f->path = path;
    f->delete_on_close = flags & FILE_FLAG_DELETE_ON_CLOSE;
    set_error(existed && (disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS) ? ERROR_ALREADY_EXISTS : 0);
    return to_handle(f);
}

void* host_thread_main(void* p) {
    auto* start = static_cast<std::pair<Thread*, std::pair<LPTHREAD_START_ROUTINE, LPVOID>>*>(p);
    Thread* t = start->first;
    const LPTHREAD_START_ROUTINE routine = start->second.first;
    const LPVOID param = start->second.second;
    delete start;
    const DWORD code = routine(param);
    {
        std::lock_guard lock(t->mutex);
        t->done = true;
        t->exit_code = code;
    }
    t->cv.notify_all();
    t->release();
    return nullptr;
}

// ---- thread-pool waits ----

struct PoolWait {
    PTP_WAIT_CALLBACK callback;
    PVOID context;
    std::mutex mutex;
    std::condition_variable cv;
    HANDLE handle = nullptr;
    std::chrono::steady_clock::time_point timeout = std::chrono::steady_clock::time_point::max();
    std::uint64_t generation = 0;
    bool closing = false;
    bool in_callback = false;
    std::thread worker;
};

void pool_wait_main(PoolWait* w) {
    std::unique_lock lock(w->mutex);
    while (!w->closing) {
        if (!w->handle) {
            w->cv.wait(lock);
            continue;
        }
        const HANDLE h = w->handle;
        const auto timeout = w->timeout;
        const std::uint64_t generation = w->generation;
        lock.unlock();
        // Sliced so that a re-arm or close is noticed; a signal is never consumed after the wait was replaced.
        DWORD result = WAIT_TIMEOUT;
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= timeout) break;
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(timeout - now).count();
            result = rthost_WaitForSingleObject(h, DWORD(std::min<long long>(left, 2)));
            if (result != WAIT_TIMEOUT) break;
            std::lock_guard check(w->mutex);
            if (w->closing || w->generation != generation) break;
        }
        lock.lock();
        if (w->closing) break;
        if (w->generation != generation) continue;
        w->handle = nullptr;
        if (result == WAIT_TIMEOUT && timeout == std::chrono::steady_clock::time_point::max()) continue;
        w->in_callback = true;
        lock.unlock();
        w->callback(nullptr, w->context, reinterpret_cast<PTP_WAIT>(w), result == WAIT_OBJECT_0 ? WAIT_OBJECT_0 : WAIT_TIMEOUT);
        lock.lock();
        w->in_callback = false;
        w->cv.notify_all();
    }
}

std::once_flag g_veh_once;

}  // namespace

std::string rthost_wstr::utf8(int count) const { return utf8_of(*this, count); }

namespace rthost {

std::string to_utf8(const wchar_t* text, int count) {
    std::string out;
    if (!text) return out;
    const std::size_t n = count < 0 ? length_of(text) : std::size_t(count);
    for (std::size_t i = 0; i < n; ++i) append_utf8(out, char32_t(text[i]));
    return out;
}

std::string to_utf8(const char16_t* text, int count) {
    std::string out;
    if (!text) return out;
    const std::size_t n = count < 0 ? length_of(text) : std::size_t(count);
    for (std::size_t i = 0; i < n; ++i) {
        char32_t c = text[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && text[i + 1] >= 0xdc00 && text[i + 1] < 0xe000)
            c = 0x10000 + ((c - 0xd800) << 10) + (text[++i] - 0xdc00);
        append_utf8(out, c);
    }
    return out;
}

std::wstring to_wide(const char* utf8) {
    std::wstring out;
    if (!utf8) return out;
    for (char32_t c : decode_utf8(utf8, std::strlen(utf8))) out += wchar_t(c);
    return out;
}

std::string host_path(const std::string& path) {
    char out[4096];
    if (gta6rt_path_to_host(path.c_str(), out, sizeof out) == GTA6RT_OK) return out;
    std::string p = path;
    if (p.rfind("\\\\?\\", 0) == 0) p.erase(0, 4);
    std::replace(p.begin(), p.end(), '\\', '/');
    if (p.size() >= 2 && (p[0] == 'Z' || p[0] == 'z') && p[1] == ':') p.erase(0, 2);
    return p;
}

// In pieces the kernel copies by value (at most 32 KiB): a larger copy cuts the title's range into copy-on-write VM
// objects at 4 KiB, and under Rosetta Metal refuses a host-memory import over a 16 KiB page where two objects meet.
bool safe_copy(void* to, const void* from, std::size_t bytes) {
    constexpr std::size_t by_value = 32 * 1024;
    for (std::size_t done = 0; done < bytes;) {
        const std::size_t piece = std::min(by_value, bytes - done);
        mach_vm_size_t copied = 0;
        if (mach_vm_read_overwrite(mach_task_self(), mach_vm_address_t(static_cast<const char*>(from) + done), piece,
                                   mach_vm_address_t(static_cast<char*>(to) + done), &copied) != KERN_SUCCESS ||
            copied != piece)
            return false;
        done += piece;
    }
    return true;
}

bool readable(const void* from, std::size_t bytes) { return range_has(from, bytes, false); }
bool writable(void* to, std::size_t bytes) { return range_has(to, bytes, true); }
bool guarded_copy(void* to, const void* from, std::size_t bytes) { return fault_guard::copy(to, from, bytes); }
int guarded_compare(const void* a, const void* b, std::size_t bytes) { return fault_guard::compare(a, b, bytes); }
std::uint64_t fault_guard_recovered() { return fault_guard::recovered(); }
bool guarded_gather(void* to, const void* from, std::size_t count, std::size_t stride) {
    return fault_guard::gather(to, from, count, stride);
}

}  // namespace rthost

DWORD rthost_GetLastError() { return t_last_error; }
void rthost_SetLastError(DWORD code) { t_last_error = code; }

DWORD rthost_GetEnvironmentVariableA(LPCSTR name, LPSTR buffer, DWORD size) {
    const char* value = name ? gta6rt_env_get(name) : nullptr;
    if (!value && name) value = std::getenv(name);
    if (!value) {
        set_error(ERROR_ENVVAR_NOT_FOUND);
        if (buffer && size) buffer[0] = 0;
        return 0;
    }
    const std::size_t n = std::strlen(value);
    if (!buffer || n + 1 > size) return DWORD(n + 1);
    std::memcpy(buffer, value, n + 1);
    return DWORD(n);
}

DWORD rthost_GetEnvironmentVariableW(rthost_wstr name, rthost_wbuf buffer, DWORD size) {
    const std::string key = name.utf8();
    const char* value = gta6rt_env_get(key.c_str());
    if (!value) value = std::getenv(key.c_str());
    if (!value) {
        set_error(ERROR_ENVVAR_NOT_FOUND);
        if (size && buffer.w) buffer.w[0] = 0;
        if (size && buffer.u) buffer.u[0] = 0;
        return 0;
    }
    return put_wide(value, buffer, buffer.null() ? 0 : size);
}

BOOL rthost_QueryPerformanceCounter(LARGE_INTEGER* out) {
    if (!out) return FALSE;
    static mach_timebase_info_data_t base = [] {
        mach_timebase_info_data_t b{};
        mach_timebase_info(&b);
        return b;
    }();
    const unsigned __int128 ns = (unsigned __int128)mach_absolute_time() * base.numer / base.denom;
    out->QuadPart = LONGLONG(ns / 100);
    return TRUE;
}

BOOL rthost_QueryPerformanceFrequency(LARGE_INTEGER* out) {
    if (!out) return FALSE;
    out->QuadPart = 10000000;
    return TRUE;
}

ULONGLONG rthost_GetTickCount64() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000ull; }
DWORD rthost_GetTickCount() { return DWORD(rthost_GetTickCount64()); }

void rthost_GetSystemTimeAsFileTime(LPFILETIME out) {
    if (!out) return;
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    const std::uint64_t t = (std::uint64_t(ts.tv_sec) + 11644473600ull) * 10000000ull + std::uint64_t(ts.tv_nsec) / 100;
    out->dwLowDateTime = DWORD(t);
    out->dwHighDateTime = DWORD(t >> 32);
}

void rthost_GetLocalTime(LPSYSTEMTIME out) {
    if (!out) return;
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    tm local{};
    localtime_r(&ts.tv_sec, &local);
    out->wYear = WORD(local.tm_year + 1900);
    out->wMonth = WORD(local.tm_mon + 1);
    out->wDayOfWeek = WORD(local.tm_wday);
    out->wDay = WORD(local.tm_mday);
    out->wHour = WORD(local.tm_hour);
    out->wMinute = WORD(local.tm_min);
    out->wSecond = WORD(local.tm_sec);
    out->wMilliseconds = WORD(ts.tv_nsec / 1000000);
}

void rthost_Sleep(DWORD ms) {
    if (ms == 0) std::this_thread::yield();
    else std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
DWORD rthost_SleepEx(DWORD ms, BOOL) {
    rthost_Sleep(ms);
    return 0;
}
BOOL rthost_SwitchToThread() {
    std::this_thread::yield();
    return TRUE;
}

DWORD rthost_GetCurrentThreadId() {
    if (gta6rt_thread* t = gta6rt_thread_current())
        if (const std::uint32_t id = gta6rt_thread_id(t)) return id;
    std::uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return DWORD(id);
}
DWORD rthost_GetCurrentProcessId() { return DWORD(getpid()); }
HANDLE rthost_GetCurrentThread() { return kCurrentThread; }
HANDLE rthost_GetCurrentProcess() { return kCurrentProcess; }

HANDLE rthost_CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID param, DWORD flags,
                           LPDWORD id) {
    if (!start) {
        set_error(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (flags & CREATE_SUSPENDED) std::fprintf(stderr, "[rt] win32_host: CreateThread(CREATE_SUSPENDED) starts at once\n");
    auto* t = new Thread;
    t->retain();  // the thread's own reference
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, std::max<std::size_t>(stack ? stack : (8u << 20), PTHREAD_STACK_MIN));
    auto* arg = new std::pair<Thread*, std::pair<LPTHREAD_START_ROUTINE, LPVOID>>(t, {start, param});
    const int e = pthread_create(&t->pthread, &attr, &host_thread_main, arg);
    pthread_attr_destroy(&attr);
    if (e) {
        delete arg;
        t->release();
        t->release();
        set_error(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    pthread_detach(t->pthread);
    if (id) {
        std::uint64_t tid = 0;
        pthread_threadid_np(t->pthread, &tid);
        *id = DWORD(tid);
    }
    return to_handle(t);
}

BOOL rthost_SetThreadPriority(HANDLE, int) { return TRUE; }

HRESULT rthost_SetThreadDescription(HANDLE thread, rthost_wstr name) {
    if (thread == kCurrentThread) pthread_setname_np(name.utf8().substr(0, 63).c_str());
    return S_OK;
}

// SRWLOCK: one word. Bit 0 = writer, the rest counts readers in steps of 2.
namespace {
std::atomic_ref<std::uintptr_t> srw(PSRWLOCK lock) { return std::atomic_ref<std::uintptr_t>(*reinterpret_cast<std::uintptr_t*>(&lock->Ptr)); }
}  // namespace

void rthost_InitializeSRWLock(PSRWLOCK lock) { lock->Ptr = nullptr; }

void rthost_AcquireSRWLockExclusive(PSRWLOCK lock) {
    auto word = srw(lock);
    for (;;) {
        std::uintptr_t expected = 0;
        if (word.compare_exchange_weak(expected, 1, std::memory_order_acquire)) return;
        word.wait(expected, std::memory_order_relaxed);
    }
}

BOOLEAN rthost_TryAcquireSRWLockExclusive(PSRWLOCK lock) {
    std::uintptr_t expected = 0;
    return srw(lock).compare_exchange_strong(expected, 1, std::memory_order_acquire);
}

void rthost_AcquireSRWLockShared(PSRWLOCK lock) {
    auto word = srw(lock);
    for (;;) {
        std::uintptr_t now = word.load(std::memory_order_relaxed);
        if (!(now & 1)) {
            if (word.compare_exchange_weak(now, now + 2, std::memory_order_acquire)) return;
            continue;
        }
        word.wait(now, std::memory_order_relaxed);
    }
}

BOOLEAN rthost_TryAcquireSRWLockShared(PSRWLOCK lock) {
    auto word = srw(lock);
    std::uintptr_t now = word.load(std::memory_order_relaxed);
    while (!(now & 1))
        if (word.compare_exchange_weak(now, now + 2, std::memory_order_acquire)) return TRUE;
    return FALSE;
}

void rthost_ReleaseSRWLockExclusive(PSRWLOCK lock) {
    auto word = srw(lock);
    word.store(0, std::memory_order_release);
    word.notify_all();
}

void rthost_ReleaseSRWLockShared(PSRWLOCK lock) {
    auto word = srw(lock);
    if (word.fetch_sub(2, std::memory_order_release) == 2) word.notify_all();
}

HANDLE rthost_CreateEventW(LPSECURITY_ATTRIBUTES, BOOL manual_reset, BOOL initial, rthost_wstr name) {
    if (!name.null()) std::fprintf(stderr, "[rt] win32_host: named event '%s' is private to this process\n", name.utf8().c_str());
    if (HANDLE h = os_event(manual_reset, initial)) return h;
    return to_handle(new Event(manual_reset, initial));
}

HANDLE rthost_CreateEventA(LPSECURITY_ATTRIBUTES attributes, BOOL manual_reset, BOOL initial, LPCSTR name) {
    return rthost_CreateEventW(attributes, manual_reset, initial, name ? rthost_wstr(name) : rthost_wstr(nullptr));
}

BOOL rthost_SetEvent(HANDLE event) {
    if (auto* e = object_as<Event>(event, Kind::event)) {
        {
            std::lock_guard lock(e->mutex);
            e->signalled = true;
        }
        e->cv.notify_all();
        return TRUE;
    }
    if (gta6rt_waitable* w = os_waitable(event)) return gta6rt_event_set(w) == GTA6RT_OK;
    set_error(ERROR_INVALID_HANDLE);
    return FALSE;
}

BOOL rthost_ResetEvent(HANDLE event) {
    if (auto* e = object_as<Event>(event, Kind::event)) {
        std::lock_guard lock(e->mutex);
        e->signalled = false;
        return TRUE;
    }
    if (gta6rt_waitable* w = os_waitable(event)) return gta6rt_event_reset(w) == GTA6RT_OK;
    set_error(ERROR_INVALID_HANDLE);
    return FALSE;
}

HANDLE rthost_CreateSemaphoreW(LPSECURITY_ATTRIBUTES, LONG initial, LONG maximum, rthost_wstr) {
    if (maximum <= 0 || initial < 0 || initial > maximum) {
        set_error(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    gta6rt_waitable* w = nullptr;
    if (gta6rt_semaphore_create(initial, maximum, &w) == GTA6RT_OK && w) {
        std::uint64_t h = 0;
        if (gta6rt_handle_alloc(GTA6RT_HANDLE_WAITABLE, w,
                                [](void* p) { gta6rt_waitable_release(static_cast<gta6rt_waitable*>(p)); }, &h) == GTA6RT_OK)
            return reinterpret_cast<HANDLE>(h);
        gta6rt_waitable_release(w);
    }
    return to_handle(new Semaphore(initial, maximum));
}

BOOL rthost_ReleaseSemaphore(HANDLE semaphore, LONG count, LPLONG previous) {
    if (auto* s = object_as<Semaphore>(semaphore, Kind::semaphore)) {
        std::lock_guard lock(s->mutex);
        if (count <= 0 || s->count + count > s->maximum) {
            set_error(ERROR_TOO_MANY_POSTS);
            return FALSE;
        }
        if (previous) *previous = s->count;
        s->count += count;
        s->cv.notify_all();
        return TRUE;
    }
    if (gta6rt_waitable* w = os_waitable(semaphore)) {
        std::int32_t before = 0;
        const bool ok = gta6rt_semaphore_release(w, count, &before) == GTA6RT_OK;
        if (ok && previous) *previous = before;
        return ok;
    }
    set_error(ERROR_INVALID_HANDLE);
    return FALSE;
}

DWORD rthost_WaitForSingleObject(HANDLE handle, DWORD ms) {
    if (Waitable* w = waitable_of(handle)) return wait_private(w, ms);
    if (gta6rt_waitable* w = os_waitable(handle)) {
        std::uint32_t index = 0;
        const gta6rt_status s = gta6rt_wait_any(&w, 1, ms_to_ns(ms), &index);
        if (s >= 0) return WAIT_OBJECT_0;
        if (s == GTA6RT_E_TIMEOUT) return WAIT_TIMEOUT;
        if (s == GTA6RT_E_ABANDONED) return WAIT_ABANDONED;
    }
    set_error(ERROR_INVALID_HANDLE);
    return WAIT_FAILED;
}

DWORD rthost_WaitForSingleObjectEx(HANDLE handle, DWORD ms, BOOL) { return rthost_WaitForSingleObject(handle, ms); }

DWORD rthost_WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL wait_all, DWORD ms) {
    if (!count || !handles || count > MAXIMUM_WAIT_OBJECTS) {
        set_error(ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }
    if (count == 1) return rthost_WaitForSingleObject(handles[0], ms);
    // Polled (1 ms slices); wait_all takes each object as it signals, not all at once as Windows does.
    const auto start = std::chrono::steady_clock::now();
    std::vector<bool> taken(count, false);
    for (;;) {
        DWORD done = 0;
        for (DWORD i = 0; i < count; ++i) {
            if (taken[i]) {
                ++done;
                continue;
            }
            const DWORD r = rthost_WaitForSingleObject(handles[i], 0);
            if (r == WAIT_FAILED) return WAIT_FAILED;
            if (r == WAIT_OBJECT_0) {
                if (!wait_all) return WAIT_OBJECT_0 + i;
                taken[i] = true;
                ++done;
            }
        }
        if (wait_all && done == count) return WAIT_OBJECT_0;
        if (ms != INFINITE && std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(ms)) return WAIT_TIMEOUT;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

BOOL rthost_CloseHandle(HANDLE handle) {
    if (handle == kCurrentProcess || handle == kCurrentThread) return TRUE;
    if (Object* o = object_of(handle)) {
        o->release();
        return TRUE;
    }
    if (handle && handle != INVALID_HANDLE_VALUE && gta6rt_handle_close(reinterpret_cast<std::uint64_t>(handle)) == GTA6RT_OK)
        return TRUE;
    set_error(ERROR_INVALID_HANDLE);
    return FALSE;
}

BOOL rthost_DuplicateHandle(HANDLE, HANDLE source, HANDLE, LPHANDLE target, DWORD, BOOL, DWORD options) {
    if (!target) {
        set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (Object* o = object_of(source)) {
        o->retain();
        *target = source;
        if (options & DUPLICATE_CLOSE_SOURCE) o->release();
        return TRUE;
    }
    std::fprintf(stderr, "[rt] win32_host: DuplicateHandle of an os/ handle is not available (os.h has no dup)\n");
    set_error(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL rthost_GetHandleInformation(HANDLE handle, LPDWORD flags) {
    if (flags) *flags = 0;
    if (object_of(handle)) return TRUE;
    if (os_waitable(handle)) return TRUE;
    set_error(ERROR_INVALID_HANDLE);
    return FALSE;
}

HANDLE rthost_CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES, rthost_wstr, DWORD flags, DWORD) {
    return to_handle(new Timer(flags & CREATE_WAITABLE_TIMER_MANUAL_RESET));
}

BOOL rthost_SetWaitableTimer(HANDLE timer, const LARGE_INTEGER* due, LONG period, PTIMERAPCROUTINE routine, LPVOID, BOOL) {
    auto* t = object_as<Timer>(timer, Kind::timer);
    if (!t || !due) {
        set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (period || routine) std::fprintf(stderr, "[rt] win32_host: SetWaitableTimer period/APC are not supported\n");
    std::chrono::steady_clock::time_point when;
    if (due->QuadPart < 0) {
        when = std::chrono::steady_clock::now() + std::chrono::nanoseconds(-due->QuadPart * 100);
    } else {
        FILETIME now{};
        rthost_GetSystemTimeAsFileTime(&now);
        const LONGLONG current = (LONGLONG(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        when = std::chrono::steady_clock::now() + std::chrono::nanoseconds(std::max<LONGLONG>(0, due->QuadPart - current) * 100);
    }
    {
        std::lock_guard lock(t->mutex);
        t->due = when;
        t->armed = true;
        t->signalled = false;
    }
    t->cv.notify_all();
    return TRUE;
}

PTP_WAIT rthost_CreateThreadpoolWait(PTP_WAIT_CALLBACK callback, PVOID context, PTP_CALLBACK_ENVIRON) {
    if (!callback) return nullptr;
    auto* w = new PoolWait{callback, context};
    w->worker = std::thread(&pool_wait_main, w);
    return reinterpret_cast<PTP_WAIT>(w);
}

void rthost_SetThreadpoolWait(PTP_WAIT wait, HANDLE handle, PFILETIME timeout) {
    auto* w = reinterpret_cast<PoolWait*>(wait);
    if (!w) return;
    std::lock_guard lock(w->mutex);
    ++w->generation;
    w->handle = handle;
    w->timeout = std::chrono::steady_clock::time_point::max();
    if (handle && timeout) {
        const LONGLONG t = (LONGLONG(timeout->dwHighDateTime) << 32) | timeout->dwLowDateTime;
        if (t <= 0) w->timeout = std::chrono::steady_clock::now() + std::chrono::nanoseconds(-t * 100);
    }
    w->cv.notify_all();
}

void rthost_WaitForThreadpoolWaitCallbacks(PTP_WAIT wait, BOOL cancel_pending) {
    auto* w = reinterpret_cast<PoolWait*>(wait);
    if (!w) return;
    std::unique_lock lock(w->mutex);
    if (cancel_pending) {
        ++w->generation;
        w->handle = nullptr;
    }
    if (w->worker.get_id() == std::this_thread::get_id()) return;
    w->cv.wait(lock, [w] { return !w->in_callback; });
}

void rthost_CloseThreadpoolWait(PTP_WAIT wait) {
    auto* w = reinterpret_cast<PoolWait*>(wait);
    if (!w) return;
    {
        std::lock_guard lock(w->mutex);
        w->closing = true;
        w->handle = nullptr;
    }
    w->cv.notify_all();
    if (w->worker.get_id() == std::this_thread::get_id()) {
        w->worker.detach();  // closed from its own callback: the worker loop sees `closing` and ends; w leaks
        return;
    }
    w->worker.join();
    delete w;
}

SIZE_T rthost_VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION out, SIZE_T length) {
    if (!out || length < sizeof(MEMORY_BASIC_INFORMATION)) {
        set_error(ERROR_BAD_LENGTH);
        return 0;
    }
    std::memset(out, 0, sizeof(*out));
    gta6rt_vm_region region{};
    const gta6rt_status s = gta6rt_vm_query(address, &region);
    if (s == GTA6RT_OK) {
        out->BaseAddress = region.base;
        out->AllocationBase = region.allocation_base;
        out->AllocationProtect = prot_to_win(region.allocation_prot);
        out->RegionSize = region.size;
        out->State = region.state == GTA6RT_VM_STATE_COMMITTED ? MEM_COMMIT
                     : region.state == GTA6RT_VM_STATE_RESERVED ? MEM_RESERVE
                                                                : MEM_FREE;
        out->Protect = region.state == GTA6RT_VM_STATE_COMMITTED ? prot_to_win(region.prot) : 0;
        out->Type = region.state == GTA6RT_VM_STATE_FREE ? 0 : region.is_image ? MEM_IMAGE : MEM_PRIVATE;
        return sizeof(*out);
    }
    if (s != GTA6RT_E_NOT_IMPLEMENTED) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    const std::uintptr_t page = vm_page_size;
    const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(address) & ~(page - 1);
    std::uintptr_t start = 0, size = 0;
    vm_prot_t prot = 0;
    if (!mach_region(at, start, size, prot) || start > at) {
        out->BaseAddress = reinterpret_cast<void*>(at);
        out->RegionSize = (start > at ? start : at + page) - at;
        out->State = MEM_FREE;
        out->Protect = PAGE_NOACCESS;
        return sizeof(*out);
    }
    std::uint32_t p = (prot & VM_PROT_READ ? GTA6RT_PROT_READ : 0) | (prot & VM_PROT_WRITE ? GTA6RT_PROT_WRITE : 0) |
                      (prot & VM_PROT_EXECUTE ? GTA6RT_PROT_EXEC : 0);
    out->BaseAddress = reinterpret_cast<void*>(at);
    out->AllocationBase = reinterpret_cast<void*>(start);
    out->RegionSize = start + size - at;
    out->State = p ? MEM_COMMIT : MEM_RESERVE;
    out->Protect = p ? prot_to_win(p) : 0;
    out->AllocationProtect = PAGE_READWRITE;
    out->Type = MEM_PRIVATE;
    return sizeof(*out);
}

BOOL rthost_VirtualProtect(LPVOID address, SIZE_T size, DWORD protect, PDWORD old_protect) {
    std::uint32_t old = 0;
    const gta6rt_status s = gta6rt_vm_protect(address, size, win_to_prot(protect), &old);
    if (s == GTA6RT_OK) {
        if (old_protect) *old_protect = prot_to_win(old);
        return TRUE;
    }
    if (s != GTA6RT_E_NOT_IMPLEMENTED) {
        set_error(ERROR_INVALID_ADDRESS);
        return FALSE;
    }
    MEMORY_BASIC_INFORMATION before{};
    rthost_VirtualQuery(address, &before, sizeof before);
    const std::uintptr_t page = vm_page_size;
    const auto begin = reinterpret_cast<std::uintptr_t>(address) & ~(page - 1);
    const auto end = (reinterpret_cast<std::uintptr_t>(address) + size + page - 1) & ~(page - 1);
    if (::mprotect(reinterpret_cast<void*>(begin), end - begin, posix_prot(win_to_prot(protect)))) {
        set_error(ERROR_INVALID_ADDRESS);
        return FALSE;
    }
    if (old_protect) *old_protect = before.Protect;
    return TRUE;
}

LPVOID rthost_VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect) {
    const std::uint32_t prot = win_to_prot(protect);
    void* base = address;
    gta6rt_status s = GTA6RT_OK;
    if (type & MEM_RESERVE) {
        s = gta6rt_vm_reserve(address, size, address ? GTA6RT_VM_FIXED : 0, &base);
        if (s == GTA6RT_OK && (type & MEM_COMMIT)) s = gta6rt_vm_commit(base, size, prot);
    } else if (type & MEM_COMMIT) {
        s = gta6rt_vm_commit(address, size, prot);
    }
    if (s == GTA6RT_OK) return base;
    if (s != GTA6RT_E_NOT_IMPLEMENTED) {
        set_error(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    if (type & MEM_RESERVE) {
        void* p = ::mmap(address, size, (type & MEM_COMMIT) ? posix_prot(prot) : PROT_NONE, MAP_PRIVATE | MAP_ANON |
                         (address ? MAP_FIXED : 0), -1, 0);
        if (p == MAP_FAILED) {
            set_error(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }
        std::lock_guard lock(g_regions_mutex);
        g_reservations[reinterpret_cast<std::uintptr_t>(p)] = size;
        return p;
    }
    if ((type & MEM_COMMIT) && address && !::mprotect(address, size, posix_prot(prot))) return address;
    set_error(ERROR_INVALID_ADDRESS);
    return nullptr;
}

BOOL rthost_VirtualFree(LPVOID address, SIZE_T size, DWORD type) {
    gta6rt_status s = (type & MEM_RELEASE) ? gta6rt_vm_release(address) : gta6rt_vm_decommit(address, size);
    if (s == GTA6RT_OK) return TRUE;
    if (s != GTA6RT_E_NOT_IMPLEMENTED) {
        set_error(ERROR_INVALID_ADDRESS);
        return FALSE;
    }
    if (type & MEM_RELEASE) {
        std::size_t length = 0;
        {
            std::lock_guard lock(g_regions_mutex);
            auto it = g_reservations.find(reinterpret_cast<std::uintptr_t>(address));
            if (it == g_reservations.end()) {
                set_error(ERROR_INVALID_ADDRESS);
                return FALSE;
            }
            length = it->second;
            g_reservations.erase(it);
        }
        return ::munmap(address, length) == 0;
    }
    ::madvise(address, size, MADV_FREE);
    return ::mprotect(address, size, PROT_NONE) == 0;
}

UINT rthost_GetWriteWatch(DWORD flags, PVOID base, SIZE_T size, PVOID* addresses, ULONG_PTR* count, LPDWORD granularity) {
    if (!count) return 1;
    std::size_t n = *count;
    if (gta6rt_vm_get_write_watch(base, size, (flags & WRITE_WATCH_FLAG_RESET) ? 1 : 0, addresses, &n) != GTA6RT_OK)
        return 1;
    *count = n;
    if (granularity) *granularity = 4096;
    return 0;
}

BOOL rthost_ReadProcessMemory(HANDLE process, LPCVOID address, LPVOID buffer, SIZE_T size, SIZE_T* read) {
    if (read) *read = 0;
    if (process != kCurrentProcess) {
        set_error(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (!rthost::safe_copy(buffer, address, size)) {
        set_error(ERROR_PARTIAL_COPY);
        return FALSE;
    }
    if (read) *read = size;
    return TRUE;
}

BOOL rthost_IsBadReadPtr(const void* address, UINT_PTR size) { return !rthost::readable(address, size); }

void rthost_GetSystemInfo(LPSYSTEM_INFO out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
#if defined(__x86_64__)
    out->wProcessorArchitecture = PROCESSOR_ARCHITECTURE_AMD64;
#else
    out->wProcessorArchitecture = PROCESSOR_ARCHITECTURE_ARM64;
#endif
    out->dwPageSize = 4096;
    out->dwAllocationGranularity = 65536;
    out->lpMinimumApplicationAddress = reinterpret_cast<LPVOID>(0x10000);
    out->lpMaximumApplicationAddress = reinterpret_cast<LPVOID>(0x7ffffffeffffull);
    int cpus = 1;
    std::size_t len = sizeof cpus;
    sysctlbyname("hw.logicalcpu", &cpus, &len, nullptr, 0);
    out->dwNumberOfProcessors = DWORD(cpus);
    out->dwActiveProcessorMask = cpus >= 64 ? ~DWORD_PTR(0) : (DWORD_PTR(1) << cpus) - 1;
}

BOOL rthost_GlobalMemoryStatusEx(LPMEMORYSTATUSEX out) {
    if (!out || out->dwLength != sizeof(MEMORYSTATUSEX)) {
        set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::uint64_t total = 0;
    std::size_t len = sizeof total;
    sysctlbyname("hw.memsize", &total, &len, nullptr, 0);
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    std::uint64_t avail = 0;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS)
        avail = (std::uint64_t(vm.free_count) + vm.inactive_count + vm.purgeable_count) * vm_page_size;
    out->ullTotalPhys = total;
    out->ullAvailPhys = std::min(avail, total);
    out->dwMemoryLoad = total ? DWORD(100 - out->ullAvailPhys * 100 / total) : 0;
    out->ullTotalPageFile = total * 2;
    out->ullAvailPageFile = out->ullAvailPhys + total;
    out->ullTotalVirtual = 0x7ffffffeffffull;
    out->ullAvailVirtual = 0x7ff000000000ull;
    return TRUE;
}

BOOL rthost_GetProcessMemoryInfo(HANDLE process, PPROCESS_MEMORY_COUNTERS out, DWORD size) {
    if (process != kCurrentProcess || !out || size < sizeof(PROCESS_MEMORY_COUNTERS)) {
        set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return FALSE;
    std::memset(out, 0, size);
    out->cb = size;
    out->WorkingSetSize = SIZE_T(info.resident_size);
    out->PeakWorkingSetSize = SIZE_T(info.resident_size_peak);
    out->PagefileUsage = SIZE_T(info.phys_footprint);
    out->PeakPagefileUsage = SIZE_T(info.ledger_phys_footprint_peak);
    if (size >= sizeof(PROCESS_MEMORY_COUNTERS) + sizeof(SIZE_T))
        *reinterpret_cast<SIZE_T*>(reinterpret_cast<char*>(out) + sizeof(PROCESS_MEMORY_COUNTERS)) =
            SIZE_T(info.phys_footprint);
    return TRUE;
}

HANDLE rthost_GetProcessHeap() {
    static Heap* heap = new Heap;
    return to_handle(heap);
}
HANDLE rthost_HeapCreate(DWORD, SIZE_T, SIZE_T) { return to_handle(new Heap); }
LPVOID rthost_HeapAlloc(HANDLE, DWORD flags, SIZE_T bytes) {
    return (flags & HEAP_ZERO_MEMORY) ? std::calloc(1, bytes ? bytes : 1) : std::malloc(bytes ? bytes : 1);
}
BOOL rthost_HeapFree(HANDLE, DWORD, LPVOID memory) {
    std::free(memory);
    return TRUE;
}

PVOID rthost_AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler) {
    const auto h = reinterpret_cast<rthost::fault_guard::VectoredHandler>(handler);
    if (rthost::fault_guard::add_vectored_handler(first != 0, h)) return reinterpret_cast<PVOID>(handler);
    std::call_once(g_veh_once, [] {
        std::fprintf(stderr, "[rt] win32_host: vectored exception handlers of the D3D12 layer are off (LEONIDA_HOST_VEH=1 "
                             "routes host write faults to them); the write guards behind them stay off\n");
    });
    set_error(ERROR_NOT_SUPPORTED);
    return nullptr;
}
ULONG rthost_RemoveVectoredExceptionHandler(PVOID cookie) {
    return rthost::fault_guard::remove_vectored_handler(reinterpret_cast<rthost::fault_guard::VectoredHandler>(cookie));
}

HANDLE rthost_CreateFileW(rthost_wstr name, DWORD access, DWORD, LPSECURITY_ATTRIBUTES, DWORD disposition, DWORD flags,
                          HANDLE) {
    if (name.null()) {
        set_error(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    return open_file(name.utf8(), access, disposition, flags);
}

HANDLE rthost_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES attributes, DWORD disposition,
                          DWORD flags, HANDLE template_file) {
    return rthost_CreateFileW(name ? rthost_wstr(name) : rthost_wstr(nullptr), access, share, attributes, disposition,
                              flags, template_file);
}

BOOL rthost_ReadFile(HANDLE file, LPVOID buffer, DWORD bytes, LPDWORD read, LPOVERLAPPED overlapped) {
    auto* f = object_as<File>(file, Kind::file);
    if (read) *read = 0;
    if (!f) {
        set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    const ssize_t n = overlapped ? ::pread(f->fd, buffer, bytes, (off_t(overlapped->OffsetHigh) << 32) | overlapped->Offset)
                                 : ::read(f->fd, buffer, bytes);
    if (n < 0) {
        set_error(errno_to_win(errno));
        return FALSE;
    }
    if (read) *read = DWORD(n);
    if (overlapped) {
        overlapped->Internal = 0;
        overlapped->InternalHigh = ULONG_PTR(n);
        if (overlapped->hEvent) rthost_SetEvent(overlapped->hEvent);
    }
    return TRUE;
}

BOOL rthost_WriteFile(HANDLE file, LPCVOID buffer, DWORD bytes, LPDWORD written, LPOVERLAPPED overlapped) {
    auto* f = object_as<File>(file, Kind::file);
    if (written) *written = 0;
    if (!f) {
        set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    const ssize_t n = overlapped ? ::pwrite(f->fd, buffer, bytes, (off_t(overlapped->OffsetHigh) << 32) | overlapped->Offset)
                                 : ::write(f->fd, buffer, bytes);
    if (n < 0) {
        set_error(errno_to_win(errno));
        return FALSE;
    }
    if (written) *written = DWORD(n);
    if (overlapped) {
        overlapped->Internal = 0;
        overlapped->InternalHigh = ULONG_PTR(n);
        if (overlapped->hEvent) rthost_SetEvent(overlapped->hEvent);
    }
    return TRUE;
}

// The title's file handles belong to os/ (file_system shim); this module only forwards title_page_watch's kernel-write
// wrappers, which are not reached from here. Refused rather than guessed.
BOOL rthost_ReadFileEx(HANDLE, LPVOID, DWORD, LPOVERLAPPED, LPOVERLAPPED_COMPLETION_ROUTINE) {
    set_error(ERROR_NOT_SUPPORTED);
    return FALSE;
}
BOOL rthost_ReadFileScatter(HANDLE, FILE_SEGMENT_ELEMENT[], DWORD, LPDWORD, LPOVERLAPPED) {
    set_error(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL rthost_SetFilePointerEx(HANDLE file, LARGE_INTEGER distance, PLARGE_INTEGER new_pointer, DWORD method) {
    auto* f = object_as<File>(file, Kind::file);
    if (!f) {
        set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    const int whence = method == FILE_CURRENT ? SEEK_CUR : method == FILE_END ? SEEK_END : SEEK_SET;
    const off_t at = ::lseek(f->fd, off_t(distance.QuadPart), whence);
    if (at < 0) {
        set_error(errno_to_win(errno));
        return FALSE;
    }
    if (new_pointer) new_pointer->QuadPart = at;
    return TRUE;
}

BOOL rthost_SetEndOfFile(HANDLE file) {
    auto* f = object_as<File>(file, Kind::file);
    if (!f) return FALSE;
    const off_t at = ::lseek(f->fd, 0, SEEK_CUR);
    return at >= 0 && ::ftruncate(f->fd, at) == 0;
}

BOOL rthost_GetFileSizeEx(HANDLE file, PLARGE_INTEGER size) {
    auto* f = object_as<File>(file, Kind::file);
    struct stat st{};
    if (!f || !size || ::fstat(f->fd, &st)) {
        set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    size->QuadPart = st.st_size;
    return TRUE;
}

BOOL rthost_FlushFileBuffers(HANDLE file) {
    auto* f = object_as<File>(file, Kind::file);
    return f && ::fsync(f->fd) == 0;
}

BOOL rthost_DeleteFileW(rthost_wstr name) {
    if (::unlink(rthost::host_path(name.utf8()).c_str()) == 0) return TRUE;
    set_error(errno_to_win(errno));
    return FALSE;
}

BOOL rthost_MoveFileExW(rthost_wstr from, rthost_wstr to, DWORD flags) {
    const std::string a = rthost::host_path(from.utf8()), b = rthost::host_path(to.utf8());
    if (!(flags & MOVEFILE_REPLACE_EXISTING)) {
        if (::renamex_np(a.c_str(), b.c_str(), RENAME_EXCL) == 0) return TRUE;
    } else if (::rename(a.c_str(), b.c_str()) == 0) {
        return TRUE;
    }
    set_error(errno == EEXIST ? ERROR_ALREADY_EXISTS : errno_to_win(errno));
    return FALSE;
}

DWORD rthost_GetFileAttributesW(rthost_wstr name) {
    struct stat st{};
    if (::stat(rthost::host_path(name.utf8()).c_str(), &st)) {
        set_error(errno_to_win(errno));
        return INVALID_FILE_ATTRIBUTES;
    }
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

BOOL rthost_CreateDirectoryW(rthost_wstr name, LPSECURITY_ATTRIBUTES) {
    if (::mkdir(rthost::host_path(name.utf8()).c_str(), 0755) == 0) return TRUE;
    set_error(errno == EEXIST ? ERROR_ALREADY_EXISTS : errno_to_win(errno));
    return FALSE;
}

HANDLE rthost_CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES, DWORD protect, DWORD size_high, DWORD size_low,
                                 rthost_wstr) {
    auto* f = object_as<File>(file, Kind::file);
    if (!f) {
        set_error(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    auto* m = new Mapping;
    m->fd = ::dup(f->fd);
    m->writable = (protect & 0xff) == PAGE_READWRITE || (protect & 0xff) == PAGE_EXECUTE_READWRITE;
    m->size = (std::uint64_t(size_high) << 32) | size_low;
    struct stat st{};
    ::fstat(m->fd, &st);
    if (!m->size) m->size = std::uint64_t(st.st_size);
    else if (m->writable && std::uint64_t(st.st_size) < m->size && ::ftruncate(m->fd, off_t(m->size))) {
        m->release();
        set_error(ERROR_DISK_FULL);
        return nullptr;
    }
    if (!m->size) {
        m->release();
        set_error(ERROR_FILE_INVALID);
        return nullptr;
    }
    return to_handle(m);
}

LPVOID rthost_MapViewOfFile(HANDLE mapping, DWORD access, DWORD offset_high, DWORD offset_low, SIZE_T bytes) {
    auto* m = object_as<Mapping>(mapping, Kind::mapping);
    if (!m) {
        set_error(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    const std::uint64_t offset = (std::uint64_t(offset_high) << 32) | offset_low;
    const std::size_t length = bytes ? bytes : std::size_t(m->size - offset);
    const bool write = (access & FILE_MAP_WRITE) && m->writable;
    void* p = ::mmap(nullptr, length, PROT_READ | (write ? PROT_WRITE : 0), MAP_SHARED, m->fd, off_t(offset));
    if (p == MAP_FAILED) {
        set_error(errno_to_win(errno));
        return nullptr;
    }
    std::lock_guard lock(g_regions_mutex);
    g_views[reinterpret_cast<std::uintptr_t>(p)] = length;
    return p;
}

BOOL rthost_UnmapViewOfFile(LPCVOID address) {
    std::size_t length = 0;
    {
        std::lock_guard lock(g_regions_mutex);
        auto it = g_views.find(reinterpret_cast<std::uintptr_t>(address));
        if (it == g_views.end()) {
            set_error(ERROR_INVALID_ADDRESS);
            return FALSE;
        }
        length = it->second;
        g_views.erase(it);
    }
    return ::munmap(const_cast<void*>(address), length) == 0;
}

BOOL rthost_FlushViewOfFile(LPCVOID address, SIZE_T bytes) {
    return ::msync(const_cast<void*>(address), bytes, MS_SYNC) == 0;
}

HANDLE rthost_FindFirstFileA(LPCSTR, LPWIN32_FIND_DATAA) {
    set_error(ERROR_FILE_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
}
BOOL rthost_FindNextFileA(HANDLE, LPWIN32_FIND_DATAA) {
    set_error(ERROR_NO_MORE_FILES);
    return FALSE;
}
BOOL rthost_FindClose(HANDLE) { return TRUE; }

HANDLE rthost_GetStdHandle(DWORD which) {
    static File* std_files[3] = {};
    static std::once_flag once;
    std::call_once(once, [] {
        for (int i = 0; i < 3; ++i) {
            std_files[i] = new File;
            std_files[i]->fd = i;
            std_files[i]->owns = false;
        }
    });
    switch (which) {
    case STD_INPUT_HANDLE: return to_handle(std_files[0]);
    case STD_OUTPUT_HANDLE: return to_handle(std_files[1]);
    case STD_ERROR_HANDLE: return to_handle(std_files[2]);
    default: return INVALID_HANDLE_VALUE;
    }
}

HMODULE rthost_GetModuleHandleW(rthost_wstr name) {
    if (name.null()) return reinterpret_cast<HMODULE>(const_cast<mach_header*>(_dyld_get_image_header(0)));
    set_error(ERROR_MOD_NOT_FOUND);
    return nullptr;
}
HMODULE rthost_GetModuleHandleA(LPCSTR name) { return rthost_GetModuleHandleW(name ? rthost_wstr(name) : rthost_wstr(nullptr)); }

// No Windows DLLs on the host: every LoadLibrary of this module's sources asks for a host DLL (vulkan-1, dxgi,
// d3d12, dwmapi, gdi32) whose replacement is native code here.
HMODULE rthost_LoadLibraryW(rthost_wstr) {
    set_error(ERROR_MOD_NOT_FOUND);
    return nullptr;
}
HMODULE rthost_LoadLibraryA(LPCSTR) {
    set_error(ERROR_MOD_NOT_FOUND);
    return nullptr;
}
HMODULE rthost_LoadLibraryExW(rthost_wstr name, HANDLE, DWORD) { return rthost_LoadLibraryW(name); }
HMODULE rthost_LoadLibraryExA(LPCSTR name, HANDLE, DWORD) { return rthost_LoadLibraryA(name); }
FARPROC rthost_GetProcAddress(HMODULE, LPCSTR) {
    set_error(ERROR_PROC_NOT_FOUND);
    return nullptr;
}
BOOL rthost_FreeLibrary(HMODULE) { return TRUE; }

DWORD rthost_GetModuleFileNameW(HMODULE module, rthost_wbuf buffer, DWORD size) {
    if (module && module != rthost_GetModuleHandleW(nullptr)) {
        set_error(ERROR_MOD_NOT_FOUND);
        return 0;
    }
    const std::string path = executable_path();
    const DWORD n = put_wide(path, buffer, size);
    if (n >= size) {
        set_error(ERROR_INSUFFICIENT_BUFFER);
        if (size && buffer.w) buffer.w[size - 1] = 0;
        if (size && buffer.u) buffer.u[size - 1] = 0;
        return size;
    }
    return n;
}

DWORD rthost_GetModuleFileNameA(HMODULE module, LPSTR buffer, DWORD size) {
    if (module && module != rthost_GetModuleHandleW(nullptr)) return 0;
    const std::string path = executable_path();
    if (!buffer || !size) return 0;
    const std::size_t n = std::min<std::size_t>(path.size(), size - 1);
    std::memcpy(buffer, path.data(), n);
    buffer[n] = 0;
    return DWORD(n);
}

int rthost_MultiByteToWideChar(UINT, DWORD, LPCCH text, int bytes, rthost_wbuf out, int out_chars) {
    if (!text) return 0;
    const std::size_t n = bytes < 0 ? std::strlen(text) + 1 : std::size_t(bytes);
    const std::u32string wide = decode_utf8(text, n);
    if (out.w || (!out.u && out.null())) {
        if (!out_chars || out.null()) return int(wide.size());
        if (int(wide.size()) > out_chars) {
            set_error(ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        for (std::size_t i = 0; i < wide.size(); ++i) out.w[i] = wchar_t(wide[i]);
        return int(wide.size());
    }
    const std::u16string u16 = to_utf16(wide);
    if (!out_chars) return int(u16.size());
    if (int(u16.size()) > out_chars) {
        set_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    std::memcpy(out.u, u16.data(), u16.size() * 2);
    return int(u16.size());
}

int rthost_WideCharToMultiByte(UINT, DWORD, rthost_wstr text, int chars, LPSTR out, int out_bytes, LPCCH, LPBOOL used_default) {
    if (used_default) *used_default = FALSE;
    if (text.null()) return 0;
    std::string utf8 = text.utf8(chars);
    if (chars < 0) utf8.push_back('\0');
    if (!out_bytes) return int(utf8.size());
    if (int(utf8.size()) > out_bytes) {
        set_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    std::memcpy(out, utf8.data(), utf8.size());
    return int(utf8.size());
}

void rthost_OutputDebugStringA(LPCSTR text) {
    if (text) std::fputs(text, stderr);
}
void rthost_OutputDebugStringW(rthost_wstr text) { std::fputs(text.utf8().c_str(), stderr); }

BOOL rthost_OpenProcessToken(HANDLE, DWORD, PHANDLE token) {
    if (token) *token = nullptr;
    set_error(ERROR_NOT_SUPPORTED);
    return FALSE;
}
BOOL rthost_OpenThreadToken(HANDLE, DWORD, BOOL, PHANDLE token) {
    if (token) *token = nullptr;
    set_error(ERROR_NO_TOKEN);
    return FALSE;
}
BOOL rthost_LookupPrivilegeValueW(rthost_wstr, rthost_wstr, PLUID luid) {
    if (luid) *luid = LUID{};
    set_error(ERROR_NOT_SUPPORTED);
    return FALSE;
}
BOOL rthost_PrivilegeCheck(HANDLE, PPRIVILEGE_SET, LPBOOL result) {
    if (result) *result = FALSE;
    return FALSE;
}

HRESULT rthost_CoInitializeEx(LPVOID, DWORD) { return S_OK; }
void rthost_CoUninitialize() {}
HRESULT rthost_CoCreateInstance(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID* out) {
    if (out) *out = nullptr;
    return REGDB_E_CLASSNOTREG;
}

BOOL rthost_GetClientRect(HWND window, LPRECT rect) {
    std::uint32_t w = 0, h = 0;
    if (!rect || !gta6rt_mac_window_alive(reinterpret_cast<gta6rt_mac_window*>(window))) {
        set_error(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }
    gta6rt_mac_window_client_pixels(reinterpret_cast<gta6rt_mac_window*>(window), &w, &h);
    *rect = RECT{0, 0, LONG(w), LONG(h)};
    return TRUE;
}
BOOL rthost_IsWindow(HWND window) { return gta6rt_mac_window_alive(reinterpret_cast<gta6rt_mac_window*>(window)); }
BOOL rthost_IsWindowVisible(HWND window) {
    return gta6rt_mac_window_visible(reinterpret_cast<gta6rt_mac_window*>(window));
}
