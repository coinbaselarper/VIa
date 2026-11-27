#pragma once

/*
 * gta6rt_os: host OS primitives (S1.1). Implemented by os/. Consumed by isa/, seh/, loader/, shims_*,
 * d3d12_vulkan/, host/. The Win32-named kernel32 surface declared in compat/ (VirtualAlloc, CreateEventW,
 * ReadFile, ...) is also implemented by os/, on top of these functions.
 * Conventions: sizes in bytes, timeouts in nanoseconds (GTA6RT_WAIT_INFINITE = forever), paths UTF-8.
 */

#include <gta6rt/abi.h>

GTA6RT_API_BEGIN

#define GTA6RT_OS_API_VERSION 4 /* 3: vm_reserve_range, memory report; 4: vm_lowest_mappable */

/* ---- virtual memory ---------------------------------------------------- */

#define GTA6RT_PROT_NONE 0u
#define GTA6RT_PROT_READ 1u
#define GTA6RT_PROT_WRITE 2u
#define GTA6RT_PROT_EXEC 4u
#define GTA6RT_PROT_GUARD 8u /* Windows PAGE_GUARD semantics, one-shot */

#define GTA6RT_VM_FIXED 1u     /* place exactly at addr or fail with GTA6RT_E_ADDRESS_IN_USE */
#define GTA6RT_VM_TOP_DOWN 2u
#define GTA6RT_VM_WRITE_WATCH 4u
#define GTA6RT_VM_LARGE_PAGES 8u /* alignment hint only on macOS */

typedef enum gta6rt_vm_state {
    GTA6RT_VM_STATE_FREE = 0,
    GTA6RT_VM_STATE_RESERVED = 1,
    GTA6RT_VM_STATE_COMMITTED = 2
} gta6rt_vm_state;

typedef struct gta6rt_vm_region {
    void* base;            /* start of the queried run of pages with identical state */
    void* allocation_base; /* reservation start */
    size_t size;
    size_t allocation_size;
    uint32_t prot;
    uint32_t allocation_prot;
    gta6rt_vm_state state;
    uint32_t is_image; /* mapped by loader/ */
} gta6rt_vm_region;

/* Reserve address space (no backing). addr may be 0. Keeps a region table so release needs only the base. */
gta6rt_status gta6rt_vm_reserve(void* addr, size_t size, uint32_t flags, void** out_base);
/* Commit pages inside a reservation. Must not wipe already-committed data (S0.3 bug #8). */
gta6rt_status gta6rt_vm_commit(void* addr, size_t size, uint32_t prot);
gta6rt_status gta6rt_vm_decommit(void* addr, size_t size);
gta6rt_status gta6rt_vm_protect(void* addr, size_t size, uint32_t prot, uint32_t* out_old_prot);
gta6rt_status gta6rt_vm_release(void* allocation_base);
gta6rt_status gta6rt_vm_query(const void* addr, gta6rt_vm_region* out);
/* Make [dst, dst+size) alias the pages at src (XMem physical pages, S0.3 mach_vm_remap design). */
gta6rt_status gta6rt_vm_alias(void* dst, const void* src, size_t size, uint32_t prot);
gta6rt_status gta6rt_vm_get_write_watch(void* base, size_t size, int reset, void** addrs, size_t* inout_count);
size_t gta6rt_vm_host_page_size(void);

/* ---- threads ----------------------------------------------------------- */

typedef struct gta6rt_thread gta6rt_thread;
typedef void (*gta6rt_thread_fn)(void* arg);

typedef struct gta6rt_thread_desc {
    gta6rt_thread_fn fn;
    void* arg;
    size_t stack_size;   /* 0 = default 8 MiB (the title's /STACK) */
    const char* name;    /* may be NULL */
    uint32_t suspended;  /* start blocked until gta6rt_thread_resume */
    uint32_t guest;      /* run the registered thread hooks (TEB attach/detach) */
} gta6rt_thread_desc;

/* Hooks run on the new thread before fn and after it returns, for threads created with guest=1 and for
 * gta6rt_thread_adopt_current(). isa/ (TEB) and loader/ (static TLS block) register through host/. */
typedef struct gta6rt_thread_hooks {
    gta6rt_status (*on_start)(gta6rt_thread* self, void* user);
    void (*on_exit)(gta6rt_thread* self, void* user);
    void* user;
} gta6rt_thread_hooks;

gta6rt_status gta6rt_thread_set_hooks(const gta6rt_thread_hooks* hooks);
gta6rt_status gta6rt_thread_create(const gta6rt_thread_desc* desc, gta6rt_thread** out);
gta6rt_status gta6rt_thread_adopt_current(int guest, gta6rt_thread** out); /* e.g. the main thread */
gta6rt_status gta6rt_thread_resume(gta6rt_thread* t);
gta6rt_status gta6rt_thread_join(gta6rt_thread* t, uint64_t timeout_ns);
void gta6rt_thread_release(gta6rt_thread* t);
gta6rt_thread* gta6rt_thread_current(void);
uint32_t gta6rt_thread_id(const gta6rt_thread* t); /* Windows-style id, unique, nonzero */
void gta6rt_thread_stack(const gta6rt_thread* t, uint64_t* out_base, uint64_t* out_limit);
void* gta6rt_thread_user_slot(gta6rt_thread* t, int slot); /* 0: isa TEB, 1: loader TLS block, 2..7 free */
void gta6rt_thread_set_user_slot(gta6rt_thread* t, int slot, void* value);
gta6rt_status gta6rt_thread_set_name(gta6rt_thread* t, const char* name);
gta6rt_status gta6rt_thread_set_priority(gta6rt_thread* t, int win32_priority);
void gta6rt_thread_yield(void);
void gta6rt_sleep_ns(uint64_t ns);

/* ---- synchronisation objects (back Win32 events/semaphores/mutexes) ------ */

#define GTA6RT_WAIT_INFINITE UINT64_MAX

typedef struct gta6rt_waitable gta6rt_waitable;

gta6rt_status gta6rt_event_create(int manual_reset, int initially_set, gta6rt_waitable** out);
gta6rt_status gta6rt_event_set(gta6rt_waitable* e);
gta6rt_status gta6rt_event_reset(gta6rt_waitable* e);
gta6rt_status gta6rt_semaphore_create(int32_t initial, int32_t maximum, gta6rt_waitable** out);
gta6rt_status gta6rt_semaphore_release(gta6rt_waitable* s, int32_t count, int32_t* out_previous);
gta6rt_status gta6rt_mutex_create(int initially_owned, gta6rt_waitable** out);
gta6rt_status gta6rt_mutex_release(gta6rt_waitable* m);
void gta6rt_waitable_retain(gta6rt_waitable* w);
void gta6rt_waitable_release(gta6rt_waitable* w);
/* Threads are waitable too (signalled on exit). */
gta6rt_waitable* gta6rt_thread_waitable(gta6rt_thread* t);
/* Returns the signalled index (>= 0), GTA6RT_E_TIMEOUT, or GTA6RT_E_ABANDONED with *out_index set. */
gta6rt_status gta6rt_wait_any(gta6rt_waitable* const* objs, uint32_t count, uint64_t timeout_ns, uint32_t* out_index);
gta6rt_status gta6rt_wait_all(gta6rt_waitable* const* objs, uint32_t count, uint64_t timeout_ns);
/* Address waits (WaitOnAddress / futex-like). */
gta6rt_status gta6rt_wait_on_address(const volatile void* addr, const void* compare, size_t size, uint64_t timeout_ns);
void gta6rt_wake_address(const void* addr, int all);

/* ---- handle table (Win32 HANDLE values) --------------------------------- */

typedef enum gta6rt_handle_kind {
    GTA6RT_HANDLE_NONE = 0,
    GTA6RT_HANDLE_WAITABLE,
    GTA6RT_HANDLE_THREAD,
    GTA6RT_HANDLE_FILE,
    GTA6RT_HANDLE_FIND,
    GTA6RT_HANDLE_MAPPING,
    GTA6RT_HANDLE_OTHER
} gta6rt_handle_kind;

/* HANDLE values handed to the guest are small multiples of 4, never pointers. */
gta6rt_status gta6rt_handle_alloc(gta6rt_handle_kind kind, void* object, void (*close)(void*), uint64_t* out_handle);
void* gta6rt_handle_lookup(uint64_t handle, gta6rt_handle_kind kind);
gta6rt_status gta6rt_handle_close(uint64_t handle);

/* ---- files ------------------------------------------------------------- */

#define GTA6RT_FILE_READ 1u
#define GTA6RT_FILE_WRITE 2u
#define GTA6RT_FILE_CREATE 4u
#define GTA6RT_FILE_TRUNCATE 8u
#define GTA6RT_FILE_EXCLUSIVE 16u
#define GTA6RT_FILE_APPEND 32u

typedef struct gta6rt_file gta6rt_file;

gta6rt_status gta6rt_file_open(const char* path_utf8, uint32_t flags, gta6rt_file** out);
gta6rt_status gta6rt_file_pread(gta6rt_file* f, void* buf, size_t size, uint64_t offset, size_t* out_read);
gta6rt_status gta6rt_file_pwrite(gta6rt_file* f, const void* buf, size_t size, uint64_t offset, size_t* out_written);
gta6rt_status gta6rt_file_size(gta6rt_file* f, uint64_t* out_size);
gta6rt_status gta6rt_file_truncate(gta6rt_file* f, uint64_t size);
gta6rt_status gta6rt_file_sync(gta6rt_file* f);
void gta6rt_file_close(gta6rt_file* f);
int gta6rt_file_fd(const gta6rt_file* f);
/* Map a read-only view of a file (loader/ uses it for the 120 MB image). */
gta6rt_status gta6rt_file_map_readonly(const char* path_utf8, const void** out_data, size_t* out_size);
void gta6rt_file_unmap(const void* data, size_t size);

/* ---- time and randomness ----------------------------------------------- */

uint64_t gta6rt_time_monotonic_ns(void);
uint64_t gta6rt_time_qpc(void);           /* QueryPerformanceCounter ticks */
uint64_t gta6rt_time_qpc_frequency(void); /* 10,000,000 */
uint64_t gta6rt_time_filetime_utc(void);  /* 100 ns units since 1601-01-01 */
gta6rt_status gta6rt_random_bytes(void* buf, size_t size);

/* ---- environment ------------------------------------------------------- */

/* Returns a pointer valid until the next set of the same name, or NULL. */
const char* gta6rt_env_get(const char* name);
gta6rt_status gta6rt_env_set(const char* name, const char* value_or_null);

/* ---- module table (GetModuleHandle / LoadLibrary / GetProcAddress) ----- */

typedef enum gta6rt_module_kind {
    GTA6RT_MODULE_GUEST_IMAGE = 1, /* PE mapped by loader/ */
    GTA6RT_MODULE_HOST_SHIM = 2    /* DLL served by host shims (kernel32, xgameruntime, d3d12, ...) */
} gta6rt_module_kind;

typedef struct gta6rt_module gta6rt_module;

/* Resolves an export of a module to a guest-callable address (0 = not found). Registered by the module's owner. */
typedef gta6rt_gva (*gta6rt_module_export_fn)(void* user, const char* name, uint32_t ordinal);

typedef struct gta6rt_module_desc {
    const char* name;      /* "kernel32.dll"; matched case-insensitively, with or without ".dll" */
    const char* path_utf8; /* host path or NULL */
    gta6rt_module_kind kind;
    gta6rt_gva base; /* HMODULE value the guest sees */
    size_t size;
    gta6rt_module_export_fn get_export;
    void* user;
} gta6rt_module_desc;

gta6rt_status gta6rt_module_register(const gta6rt_module_desc* desc, gta6rt_module** out);
gta6rt_status gta6rt_module_unregister(gta6rt_module* m);
gta6rt_module* gta6rt_module_find_by_name(const char* name_utf8);
gta6rt_module* gta6rt_module_find_by_address(gta6rt_gva addr);
gta6rt_module* gta6rt_module_find_by_base(gta6rt_gva base);
const gta6rt_module_desc* gta6rt_module_info(const gta6rt_module* m);
gta6rt_gva gta6rt_module_get_export(gta6rt_module* m, const char* name, uint32_t ordinal);
/* Enumerate in registration order; returns NULL past the end. */
gta6rt_module* gta6rt_module_at(size_t index);

/* ---- per-thread last error (Win32 GetLastError semantics) --------------- */

uint32_t gta6rt_last_error_get(void);
void gta6rt_last_error_set(uint32_t code);

/* ---- logging ----------------------------------------------------------- */

typedef enum gta6rt_log_level { GTA6RT_LOG_ERROR, GTA6RT_LOG_WARN, GTA6RT_LOG_INFO, GTA6RT_LOG_DEBUG, GTA6RT_LOG_TRACE } gta6rt_log_level;
void gta6rt_log(gta6rt_log_level level, const char* module, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
/* Async-signal-safe: no allocation, no locks, raw write(2). For fault paths. */
void gta6rt_log_signal_safe(const char* text);

/* ======================= additions in API version 2 ====================== */

/* ---- virtual memory (v2) ----------------------------------------------- */

#define GTA6RT_VM_PLACEHOLDER 16u /* reserve a placeholder (MEM_RESERVE_PLACEHOLDER): cannot be committed */
#define GTA6RT_VM_IMAGE 32u       /* region is a mapped PE image (VirtualQuery reports MEM_IMAGE) */

/* MEM_RESET: contents no longer needed, pages stay committed (madvise). */
gta6rt_status gta6rt_vm_reset(void* addr, size_t size);
/* VirtualFree(MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER): split [addr, addr+size) out of a placeholder. */
gta6rt_status gta6rt_vm_placeholder_split(void* addr, size_t size);
/* VirtualFree(MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS): merge adjacent placeholders covering exactly the range. */
gta6rt_status gta6rt_vm_placeholder_coalesce(void* addr, size_t size);
/* MEM_REPLACE_PLACEHOLDER: the placeholder [addr, addr+size) becomes an ordinary reservation. */
gta6rt_status gta6rt_vm_placeholder_replace(void* addr, size_t size, uint32_t flags);

/* Page faults that os/ must see first: PAGE_GUARD one-shots and write-watch tracking. The process signal
 * handler (isa/ for guest faults) calls this before anything else. RETRY: the page was fixed up, re-execute.
 * GUARD_PAGE: the guard was removed; raise STATUS_GUARD_PAGE_VIOLATION to the guest. */
typedef enum gta6rt_vm_fault_result {
    GTA6RT_VM_FAULT_NOT_MINE = 0,
    GTA6RT_VM_FAULT_RETRY = 1,
    GTA6RT_VM_FAULT_GUARD_PAGE = 2
} gta6rt_vm_fault_result;
gta6rt_vm_fault_result gta6rt_vm_handle_fault(const void* addr, int is_write);
/* Installs a chaining SIGSEGV/SIGBUS handler that calls gta6rt_vm_handle_fault (for host-code writes into
 * write-watched memory and for standalone use). Idempotent. Install it before isa/ installs its own. */
gta6rt_status gta6rt_os_install_fault_handler(void);

/* ---- callbacks into code os/ was handed (v2) ---------------------------- */

/* os/ calls the Win32 callback pointers it receives (CreateThread start routines, InitOnce, thread pool,
 * APCs, I/O completion routines, FLS callbacks) through this invoker. The default is a direct
 * GTA6RT_GUEST_ABI call, right for host functions on both arches and for guest code on x86_64/Rosetta.
 * isa/fexcore installs one that routes guest addresses through gta6rt_isa_call_guest. Integer args only. */
typedef uint64_t (*gta6rt_os_invoke_fn)(gta6rt_gva fn, const uint64_t* args, uint32_t count, void* user);
gta6rt_status gta6rt_os_set_invoker(gta6rt_os_invoke_fn fn, void* user);
uint64_t gta6rt_os_invoke(gta6rt_gva fn, const uint64_t* args, uint32_t count);

/* ---- path translation (v2) ---------------------------------------------- */

/* Windows/guest paths ("C:\\x", "GAME:\\x", "\\\\?\\Z:\\x", "dir\\x") become host paths:
 *  - the longest registered prefix (case-insensitive, either separator) is replaced by its host directory;
 *  - other drive letters map to <root>/<lower-case letter> when a root is set; Z: maps to "/" unless mapped;
 *  - host-absolute paths ("/Users/...") and relative paths pass through with '\\' -> '/';
 *  - "." and ".." are folded lexically, trailing dots/spaces of components are dropped (Win32 rules);
 *  - a component that does not exist with that spelling is matched case-insensitively against the disk. */
gta6rt_status gta6rt_path_set_root(const char* host_dir_utf8_or_null);
gta6rt_status gta6rt_path_map_prefix(const char* win_prefix_utf8, const char* host_dir_utf8_or_null);
gta6rt_status gta6rt_path_to_host(const char* win_path_utf8, char* out, size_t out_size);
/* Host path -> the Windows spelling the guest should see (inverse of the table above; "Z:\\..." fallback). */
gta6rt_status gta6rt_path_to_win(const char* host_path_utf8, char* out, size_t out_size);

/* ---- process (v2) ------------------------------------------------------- */

/* GetCommandLineA/W. Default: built from the host argv. */
gta6rt_status gta6rt_process_set_command_line(const char* utf8);
/* LoadLibrary* of a name that is not registered asks this hook (loader/ or shims_core/). */
typedef gta6rt_module* (*gta6rt_module_load_fn)(const char* name_utf8, uint32_t flags, void* user);
gta6rt_status gta6rt_module_set_load_hook(gta6rt_module_load_fn fn, void* user);
/* The module GetModuleHandle(NULL) returns. Default: the first GUEST_IMAGE registered. */
gta6rt_status gta6rt_module_set_main(gta6rt_module* m);

/* ======================= additions in API version 3 ====================== */

/* Reserve inside [lowest, highest_end): the first free range at or above lowest (0 = the default placement start,
 * GTA6RT_VM_BASE_HINT, 0x300000000), aligned to `alignment` (0 = 64 KiB). GTA6RT_E_NO_MEMORY when nothing fits
 * below highest_end (an x86_64 host linked with the default 4 GiB __PAGEZERO has nothing below 4 GiB).
 * flags as for gta6rt_vm_reserve (GTA6RT_VM_FIXED is ignored). */
gta6rt_status gta6rt_vm_reserve_range(uint64_t lowest, uint64_t highest_end, size_t size, size_t alignment, uint32_t flags,
                                      void** out_base);

/* Process memory, from the kernel (task_vm_info) and the vm region table. */
typedef struct gta6rt_memory_stats {
    uint64_t resident;      /* resident_size */
    uint64_t footprint;     /* phys_footprint: what memory pressure charges to the process */
    uint64_t compressed;    /* pages held by the compressor */
    uint64_t internal;      /* anonymous memory */
    uint64_t virtual_size;
    uint64_t reserved;      /* sum of the vm table's regions */
    uint64_t committed;     /* their committed pages */
    uint64_t regions;
} gta6rt_memory_stats;
gta6rt_status gta6rt_os_memory_stats(gta6rt_memory_stats* out);
/* Logs (INFO, module "os", prefix "memory:") the process totals and the largest reservations of at least
 * min_bytes (at most max_regions lines, largest resident first): base, size, committed, resident, compressed.
 * Pages aliased into several views (xmem windows) are counted in each view. GTA6RT_MEMORY_REPORT_S=N in the
 * environment makes os/ log it every N seconds from a background thread (started by the first vm reservation). */
void gta6rt_os_memory_report(const char* why, uint64_t min_bytes, uint32_t max_regions);

/* ======================= additions in API version 4 ====================== */

/* The lowest address this process can map: the end of the main executable's __PAGEZERO. At or above 4 GiB nothing
 * can be reserved below 4 GiB, ever: always on arm64 (the kernel refuses a smaller __PAGEZERO), and on x86_64 when
 * the executable was linked without -pagezero_size 0x10000 (gta6rt_os adds that to every x86_64 executable). */
uint64_t gta6rt_vm_lowest_mappable(void);

/* ---- memory accounting for the host's governor (additive) ---------------- */

/* The machine, from sysctl and host_statistics64. Counters (pageins ... decompressions) are cumulative since boot.
 * pressure_level: 1 normal, 2 warn, 4 critical (kern.memorystatus_vm_pressure_level). */
typedef struct gta6rt_system_memory {
    uint64_t physical;
    uint64_t page_size;
    uint64_t free_bytes, speculative_bytes, wired_bytes, active_bytes, inactive_bytes;
    uint64_t file_backed_bytes; /* external (file cache) pages */
    uint64_t purgeable_bytes;
    uint64_t compressor_bytes;  /* physical memory the compressor occupies */
    uint64_t compressed_bytes;  /* what it holds, uncompressed */
    uint64_t swap_total, swap_used;
    uint64_t pageins, pageouts, swapins, swapouts, compressions, decompressions, purges;
    uint32_t pressure_level;
    double load1;
} gta6rt_system_memory;
gta6rt_status gta6rt_os_system_memory(gta6rt_system_memory* out);

/* One vm-table region with what the kernel says about its pages. released_touched: resident or compressed bytes in
 * the region's no-access kernel entries (reserved or decommitted pages that still hold memory; guard pages also
 * count). kind: 1 private, 2 view, 3 image, 4 placeholder. */
typedef struct gta6rt_memory_region_usage {
    uint64_t base, size, committed, resident, compressed, released_touched;
    uint32_t kind;
    uint32_t kernel_entries;
} gta6rt_memory_region_usage;
/* Logs the same report as gta6rt_os_memory_report (with the machine state appended) and fills out[0..cap) with every
 * region's usage from that one walk; *out_count gets the number of regions (may exceed cap). With GTA6RT_MEMORY_REPORT_S
 * the host may set GTA6RT_MEMORY_REPORT_OWNER=host before the first vm reservation to stop os/'s own reporter thread
 * and call this at its interval instead. */
void gta6rt_os_memory_report_regions(const char* why, uint64_t min_bytes, uint32_t max_regions,
                                     gta6rt_memory_region_usage* out, size_t cap, size_t* out_count);

GTA6RT_API_END
