// libc-backed stand-in for the os.h / isa.h subset that shims_core calls, so its tests run before S1.1/S1.2 land.
#include "env_stub.h"

#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <gta6rt/isa.h>
#include <gta6rt/os.h>

extern "C" {

const char* gta6rt_status_name(gta6rt_status s) {
    switch (s) {
    case GTA6RT_OK: return "OK";
    case GTA6RT_E_NOT_FOUND: return "NOT_FOUND";
    case GTA6RT_E_INVALID_ARG: return "INVALID_ARG";
    default: return "ERROR";
    }
}

void gta6rt_log(gta6rt_log_level level, const char* module, const char* fmt, ...) {
    static const char* names[] = {"ERROR", "WARN", "INFO", "DEBUG", "TRACE"};
    if (level > GTA6RT_LOG_INFO && !getenv("STUB_VERBOSE")) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[%s %s] %s\n", names[level], module, buf);
}
void gta6rt_log_signal_safe(const char* text) { (void)!write(2, text, strlen(text)); }

static thread_local uint32_t t_last_error;
uint32_t gta6rt_last_error_get(void) { return t_last_error; }
void gta6rt_last_error_set(uint32_t code) { t_last_error = code; }

const char* gta6rt_env_get(const char* name) { return getenv(name); }
gta6rt_status gta6rt_env_set(const char* name, const char* value) {
    if (value) setenv(name, value, 1);
    else unsetenv(name);
    return GTA6RT_OK;
}

uint64_t gta6rt_time_monotonic_ns(void) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch()).count());
}
uint64_t gta6rt_time_qpc(void) { return gta6rt_time_monotonic_ns() / 100; }
uint64_t gta6rt_time_qpc_frequency(void) { return 10000000ull; }
uint64_t gta6rt_time_filetime_utc(void) {
    timeval tv;
    gettimeofday(&tv, nullptr);
    return (static_cast<uint64_t>(tv.tv_sec) + 11644473600ull) * 10000000ull + static_cast<uint64_t>(tv.tv_usec) * 10;
}
gta6rt_status gta6rt_random_bytes(void* buf, size_t size) {
    arc4random_buf(buf, size);
    return GTA6RT_OK;
}

void gta6rt_sleep_ns(uint64_t ns) {
    timespec ts{static_cast<time_t>(ns / 1000000000ull), static_cast<long>(ns % 1000000000ull)};
    nanosleep(&ts, nullptr);
}
void gta6rt_thread_yield(void) { sched_yield(); }

uint64_t gta6rt_os_invoke(gta6rt_gva fn, const uint64_t* args, uint32_t count) {
    using Fn = uint64_t(GTA6RT_GUEST_ABI*)(uint64_t, uint64_t, uint64_t);
    return reinterpret_cast<Fn>(static_cast<uintptr_t>(fn))(count > 0 ? args[0] : 0, count > 1 ? args[1] : 0, count > 2 ? args[2] : 0);
}

struct HandleSlot {
    gta6rt_handle_kind kind;
    void* object;
    void (*close)(void*);
};
static std::mutex g_handle_mutex;
static std::map<uint64_t, HandleSlot> g_handles;
static uint64_t g_next_handle = 4;

gta6rt_status gta6rt_handle_alloc(gta6rt_handle_kind kind, void* object, void (*close)(void*), uint64_t* out) {
    std::lock_guard lock(g_handle_mutex);
    const uint64_t h = g_next_handle;
    g_next_handle += 4;
    g_handles[h] = {kind, object, close};
    *out = h;
    return GTA6RT_OK;
}
void* gta6rt_handle_lookup(uint64_t handle, gta6rt_handle_kind kind) {
    std::lock_guard lock(g_handle_mutex);
    auto it = g_handles.find(handle);
    if (it == g_handles.end() || it->second.kind != kind) return nullptr;
    return it->second.object;
}
gta6rt_status gta6rt_handle_close(uint64_t handle) {
    HandleSlot s;
    {
        std::lock_guard lock(g_handle_mutex);
        auto it = g_handles.find(handle);
        if (it == g_handles.end()) return GTA6RT_E_NOT_FOUND;
        s = it->second;
        g_handles.erase(it);
    }
    if (s.close) s.close(s.object);
    return GTA6RT_OK;
}

struct gta6rt_file {
    int fd;
};
gta6rt_status gta6rt_file_open(const char* path, uint32_t flags, gta6rt_file** out) {
    int of = 0;
    if ((flags & GTA6RT_FILE_READ) && (flags & GTA6RT_FILE_WRITE)) of = O_RDWR;
    else if (flags & GTA6RT_FILE_WRITE) of = O_WRONLY;
    else of = O_RDONLY;
    if (flags & GTA6RT_FILE_CREATE) of |= O_CREAT;
    if (flags & GTA6RT_FILE_TRUNCATE) of |= O_TRUNC;
    if (flags & GTA6RT_FILE_EXCLUSIVE) of |= O_EXCL;
    if (flags & GTA6RT_FILE_APPEND) of |= O_APPEND;
    const int fd = open(path, of, 0644);
    if (fd < 0) return GTA6RT_E_IO;
    *out = new gta6rt_file{fd};
    return GTA6RT_OK;
}
gta6rt_status gta6rt_file_pread(gta6rt_file* f, void* buf, size_t size, uint64_t off, size_t* out) {
    const ssize_t n = pread(f->fd, buf, size, static_cast<off_t>(off));
    if (n < 0) return GTA6RT_E_IO;
    *out = static_cast<size_t>(n);
    return GTA6RT_OK;
}
gta6rt_status gta6rt_file_pwrite(gta6rt_file* f, const void* buf, size_t size, uint64_t off, size_t* out) {
    const ssize_t n = pwrite(f->fd, buf, size, static_cast<off_t>(off));
    if (n < 0) return GTA6RT_E_IO;
    *out = static_cast<size_t>(n);
    return GTA6RT_OK;
}
gta6rt_status gta6rt_file_size(gta6rt_file* f, uint64_t* out) {
    off_t end = lseek(f->fd, 0, SEEK_END);
    if (end < 0) return GTA6RT_E_IO;
    *out = static_cast<uint64_t>(end);
    return GTA6RT_OK;
}
gta6rt_status gta6rt_file_truncate(gta6rt_file* f, uint64_t size) { return ftruncate(f->fd, static_cast<off_t>(size)) == 0 ? GTA6RT_OK : GTA6RT_E_IO; }
gta6rt_status gta6rt_file_sync(gta6rt_file* f) { return fsync(f->fd) == 0 ? GTA6RT_OK : GTA6RT_E_IO; }
void gta6rt_file_close(gta6rt_file* f) {
    close(f->fd);
    delete f;
}
int gta6rt_file_fd(const gta6rt_file* f) { return f->fd; }

struct gta6rt_module {
    std::string name, path;
    gta6rt_module_desc desc;
};
static std::mutex g_mod_mutex;
static std::vector<gta6rt_module*> g_modules;
gta6rt_status gta6rt_module_register(const gta6rt_module_desc* d, gta6rt_module** out) {
    auto* m = new gta6rt_module;
    m->name = d->name ? d->name : "";
    m->path = d->path_utf8 ? d->path_utf8 : "";
    m->desc = *d;
    m->desc.name = m->name.c_str();
    m->desc.path_utf8 = m->path.c_str();
    std::lock_guard lock(g_mod_mutex);
    g_modules.push_back(m);
    if (out) *out = m;
    return GTA6RT_OK;
}
gta6rt_status gta6rt_module_unregister(gta6rt_module* m) {
    std::lock_guard lock(g_mod_mutex);
    for (auto it = g_modules.begin(); it != g_modules.end(); ++it)
        if (*it == m) {
            g_modules.erase(it);
            delete m;
            return GTA6RT_OK;
        }
    return GTA6RT_E_NOT_FOUND;
}
static std::string strip_dll(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".dll") == 0) s.resize(s.size() - 4);
    return s;
}
gta6rt_module* gta6rt_module_find_by_name(const char* name) {
    std::lock_guard lock(g_mod_mutex);
    for (auto* m : g_modules)
        if (strip_dll(m->name) == strip_dll(name)) return m;
    return nullptr;
}
gta6rt_module* gta6rt_module_find_by_address(gta6rt_gva addr) {
    std::lock_guard lock(g_mod_mutex);
    for (auto* m : g_modules)
        if (addr >= m->desc.base && addr < m->desc.base + m->desc.size) return m;
    return nullptr;
}
gta6rt_module* gta6rt_module_find_by_base(gta6rt_gva base) {
    std::lock_guard lock(g_mod_mutex);
    for (auto* m : g_modules)
        if (m->desc.base == base) return m;
    return nullptr;
}
const gta6rt_module_desc* gta6rt_module_info(const gta6rt_module* m) { return &m->desc; }
gta6rt_gva gta6rt_module_get_export(gta6rt_module* m, const char* name, uint32_t ordinal) {
    return m->desc.get_export ? m->desc.get_export(m->desc.user, name, ordinal) : 0;
}
gta6rt_module* gta6rt_module_at(size_t i) {
    std::lock_guard lock(g_mod_mutex);
    return i < g_modules.size() ? g_modules[i] : nullptr;
}

}  // extern "C"

namespace stub {
static std::mutex g_thunk_mutex;
static std::vector<ThunkRecord> g_thunks;
static constexpr gta6rt_gva kThunkBase = 0x7000000000ull;
const ThunkRecord* thunk(gta6rt_gva addr) {
    std::lock_guard lock(g_thunk_mutex);
    if (addr < kThunkBase) return nullptr;
    const size_t i = static_cast<size_t>((addr - kThunkBase) / 16);
    return i < g_thunks.size() ? &g_thunks[i] : nullptr;
}
size_t thunk_count() {
    std::lock_guard lock(g_thunk_mutex);
    return g_thunks.size();
}
}  // namespace stub


// ---- virtual memory (mmap + mach_vm_remap), 64 KiB granular like the guest sees it

static std::mutex g_vm_mutex;
static std::map<uintptr_t, size_t> g_regions;  // allocation base -> size
static std::atomic<uint64_t> g_lowest_override{0};

namespace stub {
void set_lowest_mappable(uint64_t address) { g_lowest_override.store(address); }
}  // namespace stub

static int mmap_prot(uint32_t p) {
    int r = 0;
    if (p & GTA6RT_PROT_READ) r |= PROT_READ;
    if (p & GTA6RT_PROT_WRITE) r |= PROT_WRITE;
    if (p & GTA6RT_PROT_EXEC) r |= PROT_EXEC;
    return r;
}

extern "C" {

size_t gta6rt_vm_host_page_size(void) { return static_cast<size_t>(getpagesize()); }

uint64_t gta6rt_vm_lowest_mappable(void) {
    if (const uint64_t o = g_lowest_override.load()) return o;
    for (uint32_t i = 0, n = _dyld_image_count(); i < n; ++i) {
        const auto* h = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(i));
        if (!h || h->filetype != MH_EXECUTE) continue;
        const auto* lc = reinterpret_cast<const load_command*>(h + 1);
        for (uint32_t c = 0; c < h->ncmds; ++c, lc = reinterpret_cast<const load_command*>(reinterpret_cast<const char*>(lc) + lc->cmdsize)) {
            if (lc->cmd != LC_SEGMENT_64) continue;
            const auto* seg = reinterpret_cast<const segment_command_64*>(lc);
            if (strcmp(seg->segname, "__PAGEZERO") == 0) return seg->vmaddr + seg->vmsize;
        }
        return 0;
    }
    return 0;
}

gta6rt_status gta6rt_vm_reserve_range(uint64_t lowest, uint64_t highest_end, size_t size, size_t alignment, uint32_t flags,
                                      void** out_base) {
    const size_t gran = (flags & GTA6RT_VM_LARGE_PAGES) ? (2u << 20) : (alignment > 0x10000 ? alignment : 0x10000);
    size = (size + 0xffff) & ~size_t{0xffff};
    if (const uint64_t o = g_lowest_override.load(); o && highest_end <= o) return GTA6RT_E_NO_MEMORY;
    mach_vm_address_t a = lowest ? lowest : 0x300000000ull;
    if (mach_vm_map(mach_task_self(), &a, size, gran - 1, VM_FLAGS_ANYWHERE, MEMORY_OBJECT_NULL, 0, FALSE, VM_PROT_NONE,
                    VM_PROT_ALL, VM_INHERIT_COPY) != KERN_SUCCESS)
        return GTA6RT_E_NO_MEMORY;
    if (a + size > highest_end) {
        mach_vm_deallocate(mach_task_self(), a, size);
        return GTA6RT_E_NO_MEMORY;
    }
    std::lock_guard lock(g_vm_mutex);
    g_regions[static_cast<uintptr_t>(a)] = size;
    *out_base = reinterpret_cast<void*>(a);
    return GTA6RT_OK;
}

gta6rt_status gta6rt_vm_reserve(void* addr, size_t size, uint32_t flags, void** out_base) {
    const size_t gran = (flags & GTA6RT_VM_LARGE_PAGES) ? (2u << 20) : 0x10000;
    size = (size + gran - 1) & ~(gran - 1);
    std::lock_guard lock(g_vm_mutex);
    if ((flags & GTA6RT_VM_FIXED) && addr) {
        const uintptr_t a = reinterpret_cast<uintptr_t>(addr);
        for (auto& [b, s] : g_regions)
            if (a < b + s && a + size > b) return GTA6RT_E_ADDRESS_IN_USE;
        void* p = mmap(addr, size, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
        if (p == MAP_FAILED) return GTA6RT_E_NO_MEMORY;
        g_regions[a] = size;
        *out_base = addr;
        return GTA6RT_OK;
    }
    void* raw = mmap(nullptr, size + gran, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (raw == MAP_FAILED) return GTA6RT_E_NO_MEMORY;
    const uintptr_t r = reinterpret_cast<uintptr_t>(raw);
    const uintptr_t aligned = (r + gran - 1) & ~(gran - 1);
    if (aligned > r) munmap(raw, aligned - r);
    const uintptr_t tail = aligned + size;
    if (r + size + gran > tail) munmap(reinterpret_cast<void*>(tail), r + size + gran - tail);
    g_regions[aligned] = size;
    *out_base = reinterpret_cast<void*>(aligned);
    return GTA6RT_OK;
}

gta6rt_status gta6rt_vm_commit(void* addr, size_t size, uint32_t prot) {
    const size_t pg = gta6rt_vm_host_page_size();
    const uintptr_t a = reinterpret_cast<uintptr_t>(addr) & ~(pg - 1);
    const uintptr_t e = (reinterpret_cast<uintptr_t>(addr) + size + pg - 1) & ~(pg - 1);
    return mprotect(reinterpret_cast<void*>(a), e - a, mmap_prot(prot)) == 0 ? GTA6RT_OK : GTA6RT_E_INVALID_ARG;
}

gta6rt_status gta6rt_vm_decommit(void* addr, size_t size) {
    const size_t pg = gta6rt_vm_host_page_size();
    const uintptr_t a = reinterpret_cast<uintptr_t>(addr) & ~(pg - 1);
    const uintptr_t e = (reinterpret_cast<uintptr_t>(addr) + size + pg - 1) & ~(pg - 1);
    void* p = mmap(reinterpret_cast<void*>(a), e - a, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
    return p == MAP_FAILED ? GTA6RT_E_INVALID_ARG : GTA6RT_OK;
}

gta6rt_status gta6rt_vm_protect(void* addr, size_t size, uint32_t prot, uint32_t* old) {
    if (old) *old = GTA6RT_PROT_READ | GTA6RT_PROT_WRITE;
    return gta6rt_vm_commit(addr, size, prot);
}

gta6rt_status gta6rt_vm_release(void* base) {
    std::lock_guard lock(g_vm_mutex);
    auto it = g_regions.find(reinterpret_cast<uintptr_t>(base));
    if (it == g_regions.end()) return GTA6RT_E_NOT_FOUND;
    munmap(base, it->second);
    g_regions.erase(it);
    return GTA6RT_OK;
}

gta6rt_status gta6rt_vm_query(const void* addr, gta6rt_vm_region* out) {
    std::lock_guard lock(g_vm_mutex);
    const uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    std::memset(out, 0, sizeof *out);
    for (auto& [b, s] : g_regions)
        if (a >= b && a < b + s) {
            out->base = reinterpret_cast<void*>(a & ~uintptr_t{0xfff});
            out->allocation_base = reinterpret_cast<void*>(b);
            out->size = b + s - (a & ~uintptr_t{0xfff});
            out->allocation_size = s;
            out->state = GTA6RT_VM_STATE_COMMITTED;
            out->prot = GTA6RT_PROT_READ | GTA6RT_PROT_WRITE;
            out->allocation_prot = out->prot;
            return GTA6RT_OK;
        }
    out->state = GTA6RT_VM_STATE_FREE;
    out->base = const_cast<void*>(addr);
    out->size = 0x10000;
    return GTA6RT_OK;
}

gta6rt_status gta6rt_vm_alias(void* dst, const void* src, size_t size, uint32_t prot) {
    mach_vm_address_t target = reinterpret_cast<mach_vm_address_t>(dst);
    vm_prot_t cur, max;
    const kern_return_t kr = mach_vm_remap(mach_task_self(), &target, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                                           reinterpret_cast<mach_vm_address_t>(src), FALSE, &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) return GTA6RT_E_INVALID_ARG;
    return mprotect(dst, size, mmap_prot(prot)) == 0 ? GTA6RT_OK : GTA6RT_E_INVALID_ARG;
}

}  // extern "C"

// ---- threads, waitables

struct gta6rt_waitable {
    std::mutex m;
    std::condition_variable cv;
    bool manual = true;
    bool signalled = false;
    std::atomic<int> refs{1};
};

struct gta6rt_thread {
    pthread_t th{};
    gta6rt_thread_desc desc{};
    gta6rt_waitable done;
    uint32_t id = 0;
    void* slots[8] = {};
    std::atomic<bool> started{false};
};

static thread_local gta6rt_thread* t_current = nullptr;
static std::atomic<uint32_t> g_next_tid{100};

extern "C" {

gta6rt_status gta6rt_event_create(int manual, int initial, gta6rt_waitable** out) {
    auto* w = new gta6rt_waitable;
    w->manual = manual != 0;
    w->signalled = initial != 0;
    *out = w;
    return GTA6RT_OK;
}
gta6rt_status gta6rt_event_set(gta6rt_waitable* w) {
    {
        std::lock_guard lock(w->m);
        w->signalled = true;
    }
    w->cv.notify_all();
    return GTA6RT_OK;
}
gta6rt_status gta6rt_event_reset(gta6rt_waitable* w) {
    std::lock_guard lock(w->m);
    w->signalled = false;
    return GTA6RT_OK;
}
void gta6rt_waitable_retain(gta6rt_waitable* w) { ++w->refs; }
void gta6rt_waitable_release(gta6rt_waitable* w) {
    if (--w->refs == 0) delete w;
}
gta6rt_status gta6rt_wait_any(gta6rt_waitable* const* objs, uint32_t count, uint64_t timeout_ns, uint32_t* out_index) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeout_ns == GTA6RT_WAIT_INFINITE ? 0 : timeout_ns);
    for (;;) {
        for (uint32_t i = 0; i < count; ++i) {
            std::lock_guard lock(objs[i]->m);
            if (objs[i]->signalled) {
                if (!objs[i]->manual) objs[i]->signalled = false;
                if (out_index) *out_index = i;
                return GTA6RT_OK;
            }
        }
        if (timeout_ns != GTA6RT_WAIT_INFINITE && std::chrono::steady_clock::now() >= deadline) return GTA6RT_E_TIMEOUT;
        gta6rt_sleep_ns(200000);
    }
}

static void* thread_main(void* p) {
    auto* t = static_cast<gta6rt_thread*>(p);
    t_current = t;
    t->desc.fn(t->desc.arg);
    {
        std::lock_guard lock(t->done.m);
        t->done.signalled = true;
    }
    t->done.cv.notify_all();
    return nullptr;
}

gta6rt_status gta6rt_thread_create(const gta6rt_thread_desc* desc, gta6rt_thread** out) {
    auto* t = new gta6rt_thread;
    t->desc = *desc;
    t->id = g_next_tid.fetch_add(4);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, desc->stack_size ? desc->stack_size : (8u << 20));
    if (pthread_create(&t->th, &attr, thread_main, t) != 0) {
        delete t;
        return GTA6RT_E_NO_MEMORY;
    }
    pthread_detach(t->th);
    *out = t;
    return GTA6RT_OK;
}
gta6rt_waitable* gta6rt_thread_waitable(gta6rt_thread* t) { return &t->done; }
void gta6rt_thread_release(gta6rt_thread*) {}
gta6rt_thread* gta6rt_thread_current(void) { return t_current; }
uint32_t gta6rt_thread_id(const gta6rt_thread* t) { return t->id; }
void gta6rt_thread_stack(const gta6rt_thread*, uint64_t* base, uint64_t* limit) {
    const pthread_t self = pthread_self();
    *base = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(self));
    *limit = *base - pthread_get_stacksize_np(self);
}
gta6rt_status gta6rt_thread_set_priority(gta6rt_thread*, int) { return GTA6RT_OK; }

gta6rt_status gta6rt_isa_run_entry(gta6rt_gva entry, uint64_t a1, uint64_t, uint64_t, uint64_t* out) {
    auto fn = reinterpret_cast<uint64_t(GTA6RT_GUEST_ABI*)(uint64_t)>(static_cast<uintptr_t>(entry));
    *out = fn(a1);
    return GTA6RT_OK;
}
uint64_t gta6rt_isa_call_guest4(gta6rt_gva target, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    auto fn = reinterpret_cast<uint64_t(GTA6RT_GUEST_ABI*)(uint64_t, uint64_t, uint64_t, uint64_t)>(static_cast<uintptr_t>(target));
    return fn(a1, a2, a3, a4);
}
gta6rt_status gta6rt_isa_call_guest(gta6rt_gva target, const gta6rt_guest_args* args, gta6rt_guest_ret* out) {
    auto fn = reinterpret_cast<uint64_t(GTA6RT_GUEST_ABI*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)>(static_cast<uintptr_t>(target));
    out->rax = fn(args->args[0], args->args[1], args->args[2], args->args[3], args->args[4], args->args[5]);
    return GTA6RT_OK;
}

}  // extern "C"

extern "C" gta6rt_status gta6rt_isa_make_thunk(const gta6rt_thunk_desc* d, gta6rt_gva* out) {
    std::lock_guard lock(stub::g_thunk_mutex);
    stub::g_thunks.reserve(65536);
    stub::g_thunks.push_back({d->kind, d->fn, d->user, d->debug_name});
    *out = stub::kThunkBase + 16 * (stub::g_thunks.size() - 1);
    return GTA6RT_OK;
}
