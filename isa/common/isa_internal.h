#pragma once

#include <gta6rt/isa.h>
#include <gta6rt/nt_layout.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gta6rt::isa {

// TEB allocation: the Windows part (GTA6RT_TEB_SIZE) followed by a private area the guest never touches.
constexpr size_t kTebAlloc = 0x3000;
constexpr size_t kTebPrivate = 0x1900;
constexpr uint64_t kTebMagic = 0x3642455436415447ull;  // "GTA6TEB6"
constexpr uint32_t kRetStackDepth = 64;

struct ThreadRec;

// Layout is shared with rosetta/rosetta_asm.S (offsets below).
struct TebPrivate {
    uint64_t magic;          // 0x00
    uint32_t mode;           // 0x08 gta6rt_teb_mode
    uint32_t thread_id;      // 0x0c
    uint64_t host_tsd;       // 0x10 SWAP: host GS base
    uint64_t guest_gs;       // 0x18 SWAP: TEB address
    ThreadRec* rec;          // 0x20
    uint64_t depth;          // 0x28 native-thunk nesting under SWAP
    uint64_t scratch[8];     // 0x30
    uint64_t frames[kRetStackDepth][4];  // 0x70 {return address, rbx, rsi, rdi}
};
static_assert(offsetof(TebPrivate, mode) == 0x08);
static_assert(offsetof(TebPrivate, host_tsd) == 0x10);
static_assert(offsetof(TebPrivate, depth) == 0x28);
static_assert(offsetof(TebPrivate, scratch) == 0x30);
static_assert(offsetof(TebPrivate, frames) == 0x70);
static_assert(kTebPrivate >= GTA6RT_TEB_SIZE && kTebPrivate + sizeof(TebPrivate) <= kTebAlloc);

struct ThreadRec {
    uint8_t* teb = nullptr;
    uint32_t thread_id = 0;
    uint32_t mode = 0;
    void** tls = nullptr;          // current ThreadLocalStoragePointer array
    uint32_t tls_cap = 0;          // entries in tls if isa-owned
    uint32_t foreign_n = 0;        // entries trusted in a loader-supplied array
    bool tls_owned = false;
    std::vector<void**> tls_retired;  // arrays replaced while the thread may still read them
    uint64_t* tsd_base = nullptr;  // SHARE: this thread's TSD array (remote slot updates)
    bool auto_attached = false;
    void* backend = nullptr;
};

inline TebPrivate* teb_private(uint8_t* teb) {
    return reinterpret_cast<TebPrivate*>(teb + kTebPrivate);
}

inline void teb_put(uint8_t* teb, size_t off, uint64_t v) {
    __atomic_store_n(reinterpret_cast<uint64_t*>(teb + off), v, __ATOMIC_RELEASE);
}

inline uint64_t teb_get(const uint8_t* teb, size_t off) {
    return __atomic_load_n(reinterpret_cast<const uint64_t*>(teb + off), __ATOMIC_ACQUIRE);
}

// ---- implemented by the backend (isa/rosetta or isa/fexcore) ----------------------------------------------
// Make the TEB visible to guest code on the calling thread. preferred is the configured mode; the backend
// stores the mode it used in rec->mode.
gta6rt_status backend_plant(ThreadRec* rec, gta6rt_teb_mode preferred);
void backend_unplant(ThreadRec* rec);
// Publish a TEB field that guest code reads through GS (0x58 TLS pointer, 0x60 PEB) for rec, which may be
// another thread. The TEB copy is already updated.
void backend_publish(ThreadRec* rec, size_t teb_off, uint64_t v);
bool cpuid_sites_trapped();
void backend_cpuid_raw(uint32_t leaf, uint32_t subleaf, gta6rt_cpuid_regs* out);
uint64_t backend_xcr0_raw();
gta6rt_status backend_init(const gta6rt_isa_config* cfg);

// ---- common ----------------------------------------------------------------------------------------------
gta6rt_teb_mode configured_teb_mode();
uint64_t configured_features();
ThreadRec* current_thread();
// Attach a TEB with default parameters to a thread that has none (foreign-thread path).
gta6rt_status ensure_thread_attached();
// Signal-safe: is pc inside registered guest code (or, with none registered, does the thread have a TEB)?
bool guest_code_contains(uint64_t pc, bool thread_has_teb);
void log_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log_signal_safe(const char* s);

// CPUID policy (cpuid_policy.cpp).
void cpuid_apply_policy(uint32_t leaf, uint32_t subleaf, uint64_t features, gta6rt_cpuid_regs* r);
uint64_t policy_xcr0(uint64_t features, uint64_t host_xcr0);

// Trap sites written by the fixups (int3 over cpuid/xgetbv), looked up from the SIGTRAP handler.
enum TrapKind : uint8_t { kTrapNone = 0, kTrapCpuid = 1, kTrapXgetbv = 2 };
TrapKind trap_lookup(uint64_t site, uint8_t* out_len);
extern std::atomic<uint64_t> g_trap_count;

}  // namespace gta6rt::isa
