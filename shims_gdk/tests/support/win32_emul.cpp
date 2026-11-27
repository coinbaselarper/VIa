// Test-only emulation of the Win32 calls the GDK shims and their tests make, on std primitives and POSIX files. It
// stands in for gta6rt_os's kernel32 surface until that module is linked; it is never part of gta6rt_shims_gdk.
#include <windows.h>

#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <fcntl.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <shared_mutex>
#include <thread>
#include <vector>

#include "gdk_text.h"

namespace {

std::mutex g_mu;
std::condition_variable g_cv;
thread_local DWORD t_last_error = 0;

struct Object {
    enum class Kind { Event, Thread, File, Semaphore } kind;
    explicit Object(Kind k) : kind(k) {}
    bool manual = true;
    bool signaled = false;
    int fd = -1;
    DWORD exit_code = STILL_ACTIVE;
    std::string name;
    LONG count = 0, maximum = 0;
    ~Object() {
        if (fd >= 0) ::close(fd);
    }
};

std::map<uint64_t, std::shared_ptr<Object>> g_handles;
std::map<std::string, std::weak_ptr<Object>> g_named;
std::map<std::string, int> g_named_open;
uint64_t g_next = 0x100;

HANDLE add(std::shared_ptr<Object> o) {
    std::lock_guard lock(g_mu);
    const uint64_t h = g_next;
    g_next += 4;
    g_handles[h] = std::move(o);
    return reinterpret_cast<HANDLE>(h);
}

std::shared_ptr<Object> get_locked(HANDLE h) {
    const auto it = g_handles.find(reinterpret_cast<uint64_t>(h));
    return it == g_handles.end() ? nullptr : it->second;
}

bool signaled_locked(const Object& o) {
    if (o.kind == Object::Kind::Semaphore) return o.count > 0;
    return o.signaled;
}

void consume_locked(Object& o) {
    if (o.kind == Object::Kind::Event && !o.manual) o.signaled = false;
    if (o.kind == Object::Kind::Semaphore) --o.count;
}

DWORD wait_locked(std::unique_lock<std::mutex>& lock, DWORD count, const HANDLE* handles, bool all, DWORD ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        std::vector<std::shared_ptr<Object>> objs;
        for (DWORD i = 0; i < count; ++i) {
            auto o = get_locked(handles[i]);
            if (!o) {
                t_last_error = ERROR_INVALID_HANDLE;
                return WAIT_FAILED;
            }
            objs.push_back(std::move(o));
        }
        if (all) {
            bool every = true;
            for (const auto& o : objs) every = every && signaled_locked(*o);
            if (every) {
                for (const auto& o : objs) consume_locked(*o);
                return WAIT_OBJECT_0;
            }
        } else {
            for (DWORD i = 0; i < count; ++i)
                if (signaled_locked(*objs[i])) {
                    consume_locked(*objs[i]);
                    return WAIT_OBJECT_0 + i;
                }
        }
        if (ms == 0) return WAIT_TIMEOUT;
        if (ms == INFINITE) {
            g_cv.wait(lock);
        } else if (g_cv.wait_until(lock, deadline) == std::cv_status::timeout) {
            for (DWORD i = 0; i < count; ++i) {
                auto o = get_locked(handles[i]);
                if (o && signaled_locked(*o) && !all) {
                    consume_locked(*o);
                    return WAIT_OBJECT_0 + i;
                }
            }
            return WAIT_TIMEOUT;
        }
    }
}

struct WaitCtx {
    PTP_WAIT_CALLBACK callback = nullptr;
    PVOID context = nullptr;
    HANDLE handle = nullptr;
    DWORD generation = 0;
    int running = 0;
};

std::map<WaitCtx*, std::shared_ptr<WaitCtx>> g_waits;
std::map<void*, size_t> g_allocs;
struct SrwState {
    std::shared_mutex m;
};
std::map<void*, std::unique_ptr<SrwState>> g_srw;

struct CallbackInstance {
    WaitCtx* wait;
    bool disassociated = false;
};

}  // namespace

extern "C" {

DWORD WINAPI GetLastError(void) { return t_last_error; }
void WINAPI SetLastError(DWORD e) { t_last_error = e; }
DWORD WINAPI GetCurrentThreadId(void) {
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return static_cast<DWORD>(id);
}
DWORD WINAPI GetCurrentProcessId(void) { return static_cast<DWORD>(getpid()); }
HANDLE WINAPI GetCurrentProcess(void) { return reinterpret_cast<HANDLE>(~uintptr_t{0}); }
ULONGLONG WINAPI GetTickCount64(void) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
DWORD WINAPI GetTickCount(void) { return static_cast<DWORD>(GetTickCount64()); }
void WINAPI Sleep(DWORD ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
BOOL WINAPI SwitchToThread(void) {
    std::this_thread::yield();
    return TRUE;
}
BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER* v) {
    v->QuadPart = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 100;
    return TRUE;
}
BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER* v) {
    v->QuadPart = 10000000;
    return TRUE;
}

void WINAPI GetCurrentThreadStackLimits(PULONG_PTR low, PULONG_PTR high) {
    const pthread_t self = pthread_self();
    const auto top = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(self));
    *high = top;
    *low = top - pthread_get_stacksize_np(self);
}

HANDLE WINAPI CreateEventW(LPSECURITY_ATTRIBUTES, BOOL manual, BOOL initial, LPCWSTR name) {
    std::string n = name ? rt::gdk::to_utf8(name) : std::string();
    std::shared_ptr<Object> o;
    if (!n.empty()) {
        std::lock_guard lock(g_mu);
        o = g_named[n].lock();
        if (o) {
            t_last_error = ERROR_ALREADY_EXISTS;
            const uint64_t h = g_next;
            g_next += 4;
            g_handles[h] = o;
            ++g_named_open[n];
            return reinterpret_cast<HANDLE>(h);
        }
    }
    o = std::make_shared<Object>(Object::Kind::Event);
    o->manual = manual;
    o->signaled = initial;
    o->name = n;
    HANDLE h = add(o);
    if (!n.empty()) {
        std::lock_guard lock(g_mu);
        g_named[n] = o;
        g_named_open[n] = 1;
    }
    return h;
}
HANDLE WINAPI CreateEventA(LPSECURITY_ATTRIBUTES a, BOOL m, BOOL i, LPCSTR name) {
    return CreateEventW(a, m, i, name ? rt::gdk::to_u16(name).c_str() : nullptr);
}
HANDLE WINAPI OpenEventW(DWORD, BOOL, LPCWSTR name) {
    const std::string n = rt::gdk::to_utf8(name);
    std::lock_guard lock(g_mu);
    auto o = g_named[n].lock();
    if (!o || g_named_open[n] <= 0) {
        t_last_error = ERROR_FILE_NOT_FOUND;
        return nullptr;
    }
    const uint64_t h = g_next;
    g_next += 4;
    g_handles[h] = o;
    ++g_named_open[n];
    return reinterpret_cast<HANDLE>(h);
}
BOOL WINAPI SetEvent(HANDLE h) {
    std::lock_guard lock(g_mu);
    auto o = get_locked(h);
    if (!o || o->kind != Object::Kind::Event) {
        t_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    o->signaled = true;
    g_cv.notify_all();
    return TRUE;
}
BOOL WINAPI ResetEvent(HANDLE h) {
    std::lock_guard lock(g_mu);
    auto o = get_locked(h);
    if (!o || o->kind != Object::Kind::Event) {
        t_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    o->signaled = false;
    return TRUE;
}
HANDLE WINAPI CreateSemaphoreW(LPSECURITY_ATTRIBUTES, LONG initial, LONG maximum, LPCWSTR) {
    auto o = std::make_shared<Object>(Object::Kind::Semaphore);
    o->count = initial;
    o->maximum = maximum;
    return add(o);
}
BOOL WINAPI ReleaseSemaphore(HANDLE h, LONG n, LPLONG previous) {
    std::lock_guard lock(g_mu);
    auto o = get_locked(h);
    if (!o || o->kind != Object::Kind::Semaphore) return FALSE;
    if (previous) *previous = o->count;
    o->count += n;
    g_cv.notify_all();
    return TRUE;
}

BOOL WINAPI CloseHandle(HANDLE h) {
    std::lock_guard lock(g_mu);
    const auto it = g_handles.find(reinterpret_cast<uint64_t>(h));
    if (it == g_handles.end()) {
        t_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    if (!it->second->name.empty()) --g_named_open[it->second->name];
    g_handles.erase(it);
    return TRUE;
}

BOOL WINAPI DuplicateHandle(HANDLE, HANDLE source, HANDLE, LPHANDLE target, DWORD, BOOL, DWORD) {
    std::lock_guard lock(g_mu);
    auto o = get_locked(source);
    if (!o) {
        t_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    const uint64_t h = g_next;
    g_next += 4;
    g_handles[h] = o;
    if (!o->name.empty()) ++g_named_open[o->name];
    *target = reinterpret_cast<HANDLE>(h);
    return TRUE;
}

DWORD WINAPI WaitForSingleObject(HANDLE h, DWORD ms) {
    std::unique_lock lock(g_mu);
    return wait_locked(lock, 1, &h, false, ms);
}
DWORD WINAPI WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL) { return WaitForSingleObject(h, ms); }
DWORD WINAPI WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL all, DWORD ms) {
    std::unique_lock lock(g_mu);
    return wait_locked(lock, count, handles, all != FALSE, ms);
}

HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE fn, LPVOID param, DWORD, LPDWORD id) {
    auto o = std::make_shared<Object>(Object::Kind::Thread);
    o->signaled = false;
    HANDLE h = add(o);
    std::thread([o, fn, param] {
        const DWORD code = fn(param);
        std::lock_guard lock(g_mu);
        o->exit_code = code;
        o->signaled = true;
        g_cv.notify_all();
    }).detach();
    if (id) *id = 0;
    return h;
}
BOOL WINAPI GetExitCodeThread(HANDLE h, LPDWORD code) {
    std::lock_guard lock(g_mu);
    auto o = get_locked(h);
    if (!o || o->kind != Object::Kind::Thread) return FALSE;
    *code = o->exit_code;
    return TRUE;
}

BOOL WINAPI GlobalMemoryStatusEx(LPMEMORYSTATUSEX m) {
    uint64_t mem = 0;
    size_t len = sizeof mem;
    sysctlbyname("hw.memsize", &mem, &len, nullptr, 0);
    std::memset(reinterpret_cast<char*>(m) + 8, 0, sizeof(*m) - 8);
    m->ullTotalPhys = mem;
    m->ullAvailPhys = mem / 2;
    m->ullTotalVirtual = ~0ull;
    m->ullAvailVirtual = ~0ull;
    return TRUE;
}

// ---- files: the handle is a POSIX descriptor behind the same table ----

HANDLE WINAPI CreateFileW(LPCWSTR path, DWORD access, DWORD, LPSECURITY_ATTRIBUTES, DWORD disposition, DWORD, HANDLE) {
    const std::string p = rt::gdk::to_utf8(path);
    int flags = (access & GENERIC_WRITE) ? O_RDWR : O_RDONLY;
    if (disposition == CREATE_ALWAYS) flags |= O_CREAT | O_TRUNC;
    if (disposition == OPEN_ALWAYS) flags |= O_CREAT;
    if (disposition == CREATE_NEW) flags |= O_CREAT | O_EXCL;
    const int fd = ::open(p.c_str(), flags, 0644);
    if (fd < 0) {
        t_last_error = errno == ENOENT ? ERROR_FILE_NOT_FOUND : ERROR_ACCESS_DENIED;
        return INVALID_HANDLE_VALUE;
    }
    auto o = std::make_shared<Object>(Object::Kind::File);
    o->fd = fd;
    return add(o);
}
BOOL WINAPI GetFileSizeEx(HANDLE h, PLARGE_INTEGER size) {
    std::lock_guard lock(g_mu);
    auto o = get_locked(h);
    struct stat st{};
    if (!o || o->fd < 0 || ::fstat(o->fd, &st) != 0) {
        t_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    size->QuadPart = st.st_size;
    return TRUE;
}
BOOL WINAPI ReadFile(HANDLE h, LPVOID buffer, DWORD bytes, LPDWORD got, LPOVERLAPPED overlapped) {
    int fd;
    {
        std::lock_guard lock(g_mu);
        auto o = get_locked(h);
        if (!o || o->fd < 0) {
            t_last_error = ERROR_INVALID_HANDLE;
            return FALSE;
        }
        fd = o->fd;
    }
    const off_t offset = overlapped ? (static_cast<off_t>(overlapped->OffsetHigh) << 32) | overlapped->Offset : 0;
    const ssize_t n = overlapped ? ::pread(fd, buffer, bytes, offset) : ::read(fd, buffer, bytes);
    if (n < 0) {
        t_last_error = errno == EFAULT ? ERROR_NOACCESS : ERROR_READ_FAULT;
        return FALSE;
    }
    if (got) *got = static_cast<DWORD>(n);
    return TRUE;
}

BOOL WINAPI SetEnvironmentVariableA(LPCSTR name, LPCSTR value) {
    if (!value) return ::unsetenv(name) == 0;
    return ::setenv(name, value, 1) == 0;
}
BOOL WINAPI SetEnvironmentVariableW(LPCWSTR name, LPCWSTR value) {
    const std::string n = rt::gdk::to_utf8(name);
    if (!value) return ::unsetenv(n.c_str()) == 0;
    return ::setenv(n.c_str(), rt::gdk::to_utf8(value).c_str(), 1) == 0;
}
DWORD WINAPI GetEnvironmentVariableA(LPCSTR name, LPSTR buffer, DWORD size) {
    const char* v = std::getenv(name);
    if (!v) {
        t_last_error = ERROR_ENVVAR_NOT_FOUND;
        return 0;
    }
    const size_t n = std::strlen(v);
    if (n + 1 > size) return static_cast<DWORD>(n + 1);
    std::memcpy(buffer, v, n + 1);
    return static_cast<DWORD>(n);
}
DWORD WINAPI GetEnvironmentVariableW(LPCWSTR name, LPWSTR buffer, DWORD size) {
    const char* v = std::getenv(rt::gdk::to_utf8(name).c_str());
    if (!v) {
        t_last_error = ERROR_ENVVAR_NOT_FOUND;
        return 0;
    }
    const std::u16string w = rt::gdk::to_u16(v);
    if (w.size() + 1 > size) return static_cast<DWORD>(w.size() + 1);
    std::copy(w.begin(), w.end(), buffer);
    buffer[w.size()] = 0;
    return static_cast<DWORD>(w.size());
}
DWORD WINAPI GetTempPathW(DWORD size, LPWSTR buffer) {
    const char* dir = std::getenv("TMPDIR");
    std::string p = dir && *dir ? dir : "/tmp";
    if (p.back() != '/') p += '/';
    const std::u16string w = rt::gdk::to_u16(p);
    if (w.size() + 1 > size) return static_cast<DWORD>(w.size() + 1);
    std::copy(w.begin(), w.end(), buffer);
    buffer[w.size()] = 0;
    return static_cast<DWORD>(w.size());
}
BOOL WINAPI CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES) { return ::mkdir(path, 0755) == 0; }
BOOL WINAPI CreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES) {
    return ::mkdir(rt::gdk::to_utf8(path).c_str(), 0755) == 0;
}
BOOL WINAPI DeleteFileA(LPCSTR path) { return ::unlink(path) == 0; }
BOOL WINAPI DeleteFileW(LPCWSTR path) { return ::unlink(rt::gdk::to_utf8(path).c_str()) == 0; }
BOOL WINAPI RemoveDirectoryW(LPCWSTR path) { return ::rmdir(rt::gdk::to_utf8(path).c_str()) == 0; }

LPVOID WINAPI VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect) {
    int prot = PROT_READ | PROT_WRITE;
    if (protect == PAGE_NOACCESS || !(type & MEM_COMMIT)) prot = PROT_NONE;
    if (protect == PAGE_READONLY) prot = PROT_READ;
    void* p = ::mmap(address, size, prot, MAP_ANON | MAP_PRIVATE | (address ? MAP_FIXED : 0), -1, 0);
    (void)type;
    if (p == MAP_FAILED) return nullptr;
    std::lock_guard lock(g_mu);
    g_allocs[p] = size;
    return p;
}
BOOL WINAPI VirtualFree(LPVOID address, SIZE_T size, DWORD type) {
    if (type & MEM_RELEASE) {
        std::lock_guard lock(g_mu);
        const auto it = g_allocs.find(address);
        if (it != g_allocs.end()) {
            ::munmap(address, it->second);
            g_allocs.erase(it);
            return TRUE;
        }
    }
    return size ? ::munmap(address, size) == 0 : FALSE;
}

}  // extern "C"

static SrwState& srw(PSRWLOCK l) {
    std::lock_guard lock(g_mu);
    auto& slot = g_srw[l];
    if (!slot) slot = std::make_unique<SrwState>();
    return *slot;
}
extern "C" {
void WINAPI AcquireSRWLockExclusive(PSRWLOCK l) { srw(l).m.lock(); }
void WINAPI ReleaseSRWLockExclusive(PSRWLOCK l) { srw(l).m.unlock(); }
void WINAPI AcquireSRWLockShared(PSRWLOCK l) { srw(l).m.lock_shared(); }
void WINAPI ReleaseSRWLockShared(PSRWLOCK l) { srw(l).m.unlock_shared(); }

// ---- thread pool waits (what XTaskQueueRegisterWaiter uses) ----

PTP_WAIT WINAPI CreateThreadpoolWait(PTP_WAIT_CALLBACK cb, PVOID ctx, PTP_CALLBACK_ENVIRON) {
    auto w = std::make_shared<WaitCtx>();
    w->callback = cb;
    w->context = ctx;
    std::lock_guard lock(g_mu);
    g_waits[w.get()] = w;
    return reinterpret_cast<PTP_WAIT>(w.get());
}

void WINAPI SetThreadpoolWait(PTP_WAIT wait, HANDLE h, PFILETIME) {
    std::shared_ptr<WaitCtx> w;
    std::lock_guard lock(g_mu);
    w = g_waits[reinterpret_cast<WaitCtx*>(wait)];
    const DWORD generation = ++w->generation;
    g_cv.notify_all();
    if (!h) {
        w->handle = nullptr;
        return;
    }
    w->handle = h;
    ++w->running;
    std::thread([w, h, generation] {
        std::unique_lock lock(g_mu);
        bool signaled = false;
        for (;;) {
            if (w->generation != generation) break;
            auto o = get_locked(h);
            if (!o) break;
            if (signaled_locked(*o)) {
                consume_locked(*o);
                signaled = true;
                break;
            }
            g_cv.wait(lock);
        }
        if (signaled && w->generation == generation) {
            w->handle = nullptr;
            CallbackInstance instance{w.get()};
            lock.unlock();
            w->callback(reinterpret_cast<PTP_CALLBACK_INSTANCE>(&instance), w->context,
                        reinterpret_cast<PTP_WAIT>(w.get()), WAIT_OBJECT_0);
            lock.lock();
            if (!instance.disassociated) --w->running;
        } else {
            --w->running;
        }
        g_cv.notify_all();
    }).detach();
}

void WINAPI DisassociateCurrentThreadFromCallback(PTP_CALLBACK_INSTANCE pci) {
    auto* instance = reinterpret_cast<CallbackInstance*>(pci);
    std::lock_guard lock(g_mu);
    if (instance && !instance->disassociated) {
        instance->disassociated = true;
        --instance->wait->running;
        g_cv.notify_all();
    }
}

void WINAPI WaitForThreadpoolWaitCallbacks(PTP_WAIT wait, BOOL cancel) {
    std::unique_lock lock(g_mu);
    auto w = g_waits[reinterpret_cast<WaitCtx*>(wait)];
    if (cancel) {
        ++w->generation;
        w->handle = nullptr;
        g_cv.notify_all();
    }
    g_cv.wait(lock, [&] { return w->running == 0; });
}

void WINAPI CloseThreadpoolWait(PTP_WAIT wait) {
    std::unique_lock lock(g_mu);
    auto w = g_waits[reinterpret_cast<WaitCtx*>(wait)];
    ++w->generation;
    g_cv.notify_all();
    g_cv.wait(lock, [&] { return w->running == 0; });
    g_waits.erase(w.get());
}

}  // extern "C"
