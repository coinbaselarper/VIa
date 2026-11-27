#pragma once

#include <gta6rt/nt_layout.h>
#include <gta6rt/os.h>

// System headers first: compat's sal.h defines names (__reserved, ...) that the SDK uses as identifiers.
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/mount.h>
#include <copyfile.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <libkern/OSCacheControl.h>
#include <malloc/malloc.h>
#include <mach-o/dyld.h>
#include <crt_externs.h>
#include <sys/resource.h>
#include <sched.h>

#include <windows.h>

#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace gta6rt_os {

// ---- core.cpp ----
void set_error(uint32_t code);
uint32_t errno_to_win32(int e);
gta6rt_status errno_to_status(int e);
uint32_t status_to_win32(gta6rt_status s);
bool log_enabled(gta6rt_log_level lvl);

std::string u16_to_utf8(const char16_t* s, size_t n = (size_t)-1);
std::u16string utf8_to_u16(const char* s, size_t n = (size_t)-1);
size_t u16_len(const char16_t* s);
// Copies src into a Win32 caller buffer of cap WCHARs. Returns the Win32 result: chars copied (no NUL) on
// success, or required size including NUL when it does not fit.
DWORD copy_out_w(const std::u16string& src, LPWSTR dst, DWORD cap);
DWORD copy_out_a(const std::string& src, LPSTR dst, DWORD cap);

uint64_t monotonic_ns();
uint64_t deadline_from_timeout(uint64_t timeout_ns);
uint64_t ms_to_ns(DWORD ms);
uint64_t filetime_now();

// ---- vm.cpp ----
uint32_t win32_to_prot(DWORD protect, bool* ok);
DWORD prot_to_win32(uint32_t prot);
size_t host_page();
// Register a mapped view (MapViewOfFile) so VirtualQuery reports it; size is logical bytes.
void vm_register_view(void* base, size_t size, uint32_t prot);
bool vm_unregister_view(void* base, size_t* out_size);
bool vm_is_placeholder(void* base, size_t size);
int vm_region_type(const void* addr); // 0 none, 1 private, 2 mapped view, 3 image

// ---- threads.cpp ----
struct Thread;
Thread* current_thread();
uint32_t current_tid();
void thread_retain(gta6rt_thread* t);
gta6rt_thread* thread_by_id(uint32_t tid); // retained
pthread_cond_t* thread_wait_cv(gta6rt_thread* t);
// Per-thread wake word for blocking waits: a waiter reads it, re-checks its condition, then sleeps while it is
// unchanged; a waker bumps it (with the wait lock held when the word belongs to a linked waiter) and wakes.
std::atomic<uint32_t>* thread_wake_word(gta6rt_thread* t);
void wake_word(std::atomic<uint32_t>* w);
void sleep_on_word(std::atomic<uint32_t>* w, uint32_t seen, uint64_t timeout_ns); // UINT64_MAX = no timeout

struct Apc {
    Apc* next;
    void (*host_fn)(Apc* self); // when set, called instead of the guest function
    gta6rt_gva fn;
    uint64_t args[3];
    uint32_t nargs;
    void* ctx;
};
// Queue an APC; wakes the thread if it is in an alertable wait. Takes ownership of apc (deleted after run).
bool queue_apc(gta6rt_thread* t, Apc* apc);
bool queue_apc_locked(gta6rt_thread* t, Apc* apc);
// Run pending APCs of the current thread; returns number run. Must be called without the wait lock.
int run_apcs();
bool has_apcs_locked(gta6rt_thread* t); // caller holds the wait lock
void set_alertable_locked(gta6rt_thread* t, bool on);

// ---- waits.cpp ----
extern pthread_mutex_t g_wait_mx;
void waitable_init_event(gta6rt_waitable* w, bool manual, bool set);
gta6rt_waitable* waitable_new_event(bool manual, bool set);
void waitable_set_locked(gta6rt_waitable* w);
void abandon_mutexes_locked(gta6rt_thread* t);
// Win32-flavoured wait used by WaitFor*, SleepEx, GetOverlappedResultEx. Returns a WAIT_* code and sets
// last error on WAIT_FAILED.
DWORD wait_win32(gta6rt_waitable* const* objs, uint32_t n, bool all, uint64_t timeout_ns, bool alertable);
// Resolve a Win32 HANDLE (including pseudo handles and file handles) to a retained waitable.
gta6rt_waitable* handle_to_waitable(HANDLE h);

// ---- handles.cpp ----
// Internal object kinds behind GTA6RT_HANDLE_OTHER.
enum class OtherKind : uint32_t { None = 0, IoPort = 0x494f4350, Token, Power, Heap, Misc };
struct OtherObject {
    OtherKind kind;
    virtual ~OtherObject() = default;
};
uint64_t handle_alloc_internal(gta6rt_handle_kind kind, void* object, void (*close)(void*), void (*retain)(void*));
// Looks up and retains via the registered retain function; release with handle_unref.
void* handle_ref(uint64_t h, gta6rt_handle_kind kind, void (**release)(void*));
// handle_ref of any kind in one lookup; *kind is the handle's kind (NONE when invalid), even when not retained.
void* handle_ref_kind(uint64_t h, gta6rt_handle_kind* kind, void (**release)(void*));
// Keeps the handle's object alive without retaining it (one atomic on the handle entry); 0 when invalid.
void* handle_pin(uint64_t h, gta6rt_handle_kind* kind, void** object);
void handle_unpin(void* pin);
gta6rt_handle_kind handle_kind(uint64_t h);
OtherObject* handle_other(uint64_t h, OtherKind kind); // not retained
inline uint64_t H(HANDLE h) { return (uint64_t)(uintptr_t)h; }
inline HANDLE HV(uint64_t h) { return (HANDLE)(uintptr_t)h; }
constexpr uint64_t kPseudoProcess = (uint64_t)-1;
constexpr uint64_t kPseudoThread = (uint64_t)-2;

// ---- files.cpp ----
gta6rt_waitable* file_waitable(gta6rt_file* f); // retained
void file_retain(gta6rt_file* f);
std::string win_to_host_path(const char16_t* p, uint32_t* err);
std::string win_to_host_path8(const char* p, uint32_t* err);
std::u16string host_to_win_path(const std::string& host);
int file_fd_internal(gta6rt_file* f);
bool file_writable(gta6rt_file* f);
gta6rt_file* file_from_handle(HANDLE h, void (**release)(void*)); // retained

// ---- time_sys.cpp ----
uint32_t num_cpus();

} // namespace gta6rt_os

#define OS_LOG(lvl, ...) gta6rt_log(lvl, "os", __VA_ARGS__)
