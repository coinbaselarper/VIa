// isa/rosetta: the x86_64 host backend. Guest code runs natively (translated by Rosetta 2 as part of this
// process). TEB via SHARE: TEB, ThreadLocalStoragePointer and PEB are planted in the thread's own TSD slots
// 6/11/12 so gs:[0x30]/[0x58]/[0x60] read them (S0.1, NOTES S0.6). SWAP repoints GS at the TEB with
// thread_set_tsd_base around every guest/host transition. This is the only file that touches GS.

#include "isa_internal.h"
#include "sse4a.h"
#include "x86_patch.h"
#include "zen_insn.h"

#include <mach-o/dyld.h>
#include <pthread.h>
#include <signal.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace gta6rt::isa {
bool cpuid_sites_trapped();
uint64_t arena_reserve(uint64_t near, size_t size, uint64_t* out_zero32);
bool arena_write(uint64_t at, const void* code, size_t n);
bool arena_retry_write_fault(uint64_t addr);
uint64_t rosetta_zen_wait_entry(ZenWaitKind kind);
}  // namespace gta6rt::isa

using namespace gta6rt::isa;

extern "C" {
void gta6rt_rosetta_call_guest_asm(uint64_t target, const uint64_t* args, uint32_t count, gta6rt_guest_ret* out);
void gta6rt_rosetta_native_entry(void);
void gta6rt_rosetta_frame_entry(void);
}

namespace {

constexpr uint64_t kSlotTeb = GTA6RT_SHARE_TSD_SLOT_TEB * 8;
constexpr uint64_t kSlotTls = GTA6RT_SHARE_TSD_SLOT_TLS * 8;
constexpr uint64_t kSlotPeb = GTA6RT_SHARE_TSD_SLOT_PEB * 8;
static_assert(kSlotTeb == GTA6RT_TEB_SELF && kSlotTls == GTA6RT_TEB_TLS_POINTER && kSlotPeb == GTA6RT_TEB_PEB);

inline uint64_t gs_read(uint64_t off) {
    uint64_t v;
    __asm__ volatile("movq %%gs:(%1), %0" : "=r"(v) : "r"(off) : "memory");
    return v;
}

inline void gs_write(uint64_t off, uint64_t v) {
    __asm__ volatile("movq %1, %%gs:(%0)" : : "r"(off), "r"(v) : "memory");
}

inline void set_tsd_base(uint64_t base) {
    uint64_t rax = 0x3000003;
    __asm__ volatile("syscall" : "+a"(rax) : "D"(base), "S"(0ull) : "rcx", "rdx", "r11", "memory", "cc");
}

// Address of the calling thread's TSD array: slot 0 holds pthread_self, and a marker planted through GS must
// show up at +0x30. Only the struct _pthread page is scanned.
uint64_t* find_tsd_base(uint64_t marker_slot_off) {
    const uint64_t self = gs_read(0);
    const uint64_t saved = gs_read(marker_slot_off);
    const uint64_t marker = 0x6a7e5c0de0000000ull ^ reinterpret_cast<uint64_t>(&saved);
    gs_write(marker_slot_off, marker);
    uint64_t* found = nullptr;
    auto* p = reinterpret_cast<uint64_t*>(self);
    for (int i = 0; i < 0x1000 / 8; ++i) {
        if (p[i] == self && p[i + marker_slot_off / 8] == marker) {
            found = p + i;
            break;
        }
    }
    gs_write(marker_slot_off, saved);
    return found;
}

std::atomic<gta6rt_guest_fault_handler> g_fault_handler{nullptr};
void* g_fault_user = nullptr;
std::atomic<gta6rt_illegal_insn_hook> g_ill_hook{nullptr};
void* g_ill_user = nullptr;
struct sigaction g_old[32];
std::once_flag g_sig_once;
bool g_translated = false;

struct ThunkRec {
    const void* fn;  // +0, read by the asm entries
    void* user;
    gta6rt_thunk_kind kind;
    char name[96];
};

// Thunk: `movabs r10, rec; jmp [rip+0]; .quad target` (16-byte aligned, so the target slot is 8-aligned). Under
// SHARE the NATIVE entry only tail-jumps to fn, so a NATIVE thunk's slot holds fn itself. The first thread that has
// to use SWAP retargets every NATIVE slot to the entry (one aligned 8-byte store each) before it runs guest code.
// GTA6RT_ISA_DIRECT_THUNKS=0 keeps the entry from the start.
std::mutex g_thunk_mu;
std::vector<uint64_t>* g_native_slots;
bool g_entry_only = false;

bool direct_thunks_env() {
    static const bool on = [] {
        const char* v = getenv("GTA6RT_ISA_DIRECT_THUNKS");
        return !(v && v[0] == '0');
    }();
    return on;
}

void retarget_native_thunks_to_entry() {
    std::lock_guard<std::mutex> lk(g_thunk_mu);
    if (g_entry_only) return;
    g_entry_only = true;
    const uint64_t entry = reinterpret_cast<uint64_t>(&gta6rt_rosetta_native_entry);
    size_t n = 0;
    if (g_native_slots)
        for (uint64_t slot : *g_native_slots) {
            if (*reinterpret_cast<const uint64_t*>(slot) == entry) continue;
            if (arena_is_rwx()) __atomic_store_n(reinterpret_cast<uint64_t*>(slot), entry, __ATOMIC_RELEASE);
            else arena_write(slot, &entry, sizeof entry);
            ++n;
        }
    if (n) log_line("thunks: a SWAP thread appeared; %zu NATIVE thunks now go through the SWAP-aware entry", n);
}

void chain(int sig, siginfo_t* si, void* uc) {
    const struct sigaction& o = g_old[sig];
    if ((o.sa_flags & SA_SIGINFO) && o.sa_sigaction) {
        o.sa_sigaction(sig, si, uc);
        return;
    }
    if (o.sa_handler == SIG_IGN) return;
    if (o.sa_handler != SIG_DFL && o.sa_handler) {
        o.sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void ctx_from_mcontext(const _STRUCT_MCONTEXT64* mc, gta6rt_x64_context* c) {
    memset(c, 0, sizeof *c);
    const auto& s = mc->__ss;
    c->context_flags = 0x10000F;  // CONTEXT_AMD64 | CONTROL | INTEGER | SEGMENTS | FLOATING_POINT
    c->rax = s.__rax;
    c->rbx = s.__rbx;
    c->rcx = s.__rcx;
    c->rdx = s.__rdx;
    c->rdi = s.__rdi;
    c->rsi = s.__rsi;
    c->rbp = s.__rbp;
    c->rsp = s.__rsp;
    c->r8 = s.__r8;
    c->r9 = s.__r9;
    c->r10 = s.__r10;
    c->r11 = s.__r11;
    c->r12 = s.__r12;
    c->r13 = s.__r13;
    c->r14 = s.__r14;
    c->r15 = s.__r15;
    c->rip = s.__rip;
    c->eflags = static_cast<uint32_t>(s.__rflags);
    c->seg_cs = static_cast<uint16_t>(s.__cs);
    c->seg_fs = static_cast<uint16_t>(s.__fs);
    c->seg_gs = static_cast<uint16_t>(s.__gs);
    memcpy(c->fp.flt_save, &mc->__fs.__fpu_fcw, 512);
    c->mxcsr = mc->__fs.__fpu_mxcsr;
}

void ctx_to_mcontext(const gta6rt_x64_context* c, _STRUCT_MCONTEXT64* mc) {
    auto& s = mc->__ss;
    s.__rax = c->rax;
    s.__rbx = c->rbx;
    s.__rcx = c->rcx;
    s.__rdx = c->rdx;
    s.__rdi = c->rdi;
    s.__rsi = c->rsi;
    s.__rbp = c->rbp;
    s.__rsp = c->rsp;
    s.__r8 = c->r8;
    s.__r9 = c->r9;
    s.__r10 = c->r10;
    s.__r11 = c->r11;
    s.__r12 = c->r12;
    s.__r13 = c->r13;
    s.__r14 = c->r14;
    s.__r15 = c->r15;
    s.__rip = c->rip;
    s.__rflags = (s.__rflags & ~0xFFFull) | (c->eflags & 0xFFF);
    const uint32_t mask = mc->__fs.__fpu_mxcsrmask ? mc->__fs.__fpu_mxcsrmask : 0xFFBF;
    memcpy(&mc->__fs.__fpu_fcw, c->fp.flt_save, 512);
    mc->__fs.__fpu_mxcsr = c->mxcsr & mask;
    mc->__fs.__fpu_mxcsrmask = mask;
}

bool emulate_sse4a(_STRUCT_MCONTEXT64* mc) {
    const auto* ip = reinterpret_cast<const uint8_t*>(mc->__ss.__rip);
    Sse4aInsn in;
    if (!sse4a_decode(ip, 8, &in)) return false;
    uint64_t x[16][2];
    memcpy(x, &mc->__fs.__fpu_xmm0, sizeof x);
    sse4a_execute(in, x);
    memcpy(&mc->__fs.__fpu_xmm0, x, sizeof x);
    mc->__ss.__rip += in.length;
    g_trap_count.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool emulate_zen(_STRUCT_MCONTEXT64* mc) {
    const auto* ip = reinterpret_cast<const uint8_t*>(mc->__ss.__rip);
    ZenDecoded d;
    if (!zen_decode(ip, 15, &d)) return false;
    if (d.kind == ZenInsn::ClZero) {
        uint64_t a = d.addr32 ? static_cast<uint32_t>(mc->__ss.__rax) : mc->__ss.__rax;
        memset(reinterpret_cast<void*>(a & ~uint64_t(63)), 0, 64);
    }
    mc->__ss.__rip += d.length;
    g_trap_count.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool emulate_trap_site(_STRUCT_MCONTEXT64* mc) {
    const uint64_t site = mc->__ss.__rip - 1;
    uint8_t len = 0;
    const TrapKind k = trap_lookup(site, &len);
    if (k == kTrapNone) return false;
    auto& s = mc->__ss;
    if (k == kTrapCpuid) {
        gta6rt_cpuid_regs r;
        gta6rt_isa_cpuid(static_cast<uint32_t>(s.__rax), static_cast<uint32_t>(s.__rcx), &r);
        s.__rax = r.eax;
        s.__rbx = r.ebx;
        s.__rcx = r.ecx;
        s.__rdx = r.edx;
    } else {
        const uint64_t x = static_cast<uint32_t>(s.__rcx) == 0 ? policy_xcr0(configured_features(), 0) : 0;
        s.__rax = static_cast<uint32_t>(x);
        s.__rdx = static_cast<uint32_t>(x >> 32);
    }
    s.__rip = site + len;
    g_trap_count.fetch_add(1, std::memory_order_relaxed);
    return true;
}

uint32_t exception_code(int sig, const siginfo_t* si, const uint8_t* ip, gta6rt_x64_exception_record* rec,
                        const _STRUCT_MCONTEXT64* mc) {
    rec->n_params = 0;
    switch (sig) {
    case SIGSEGV:
    case SIGBUS:
        if (ip && ip[0] == 0xCD && ip[1] == 0x29) {  // __fastfail
            rec->n_params = 1;
            rec->params[0] = mc->__ss.__rcx;
            return 0xC0000409;
        }
        rec->n_params = 2;
        rec->params[0] = (mc->__es.__err & 2) ? 1 : 0;
        rec->params[1] = reinterpret_cast<uint64_t>(si->si_addr);
        return 0xC0000005;
    case SIGILL:
        return 0xC000001D;
    case SIGTRAP:
        return 0x80000003;
    case SIGFPE:
        if (si->si_code == FPE_INTDIV) return 0xC0000094;
        if (si->si_code == FPE_INTOVF) return 0xC0000095;
        if (si->si_code == FPE_FLTDIV) return 0xC000008E;
        if (si->si_code == FPE_FLTOVF) return 0xC0000091;
        if (si->si_code == FPE_FLTUND) return 0xC0000093;
        if (si->si_code == FPE_FLTRES) return 0xC000008F;
        return 0xC0000090;
    default:
        return 0xC0000001;
    }
}

void on_signal(int sig, siginfo_t* si, void* ucv) {
    const uint64_t tok = gta6rt_isa_enter_host_from_signal();
    auto* uc = static_cast<ucontext_t*>(ucv);
    _STRUCT_MCONTEXT64* mc = uc->uc_mcontext;
    if ((sig == SIGBUS || sig == SIGSEGV) && (mc->__es.__err & 2) &&
        arena_retry_write_fault(reinterpret_cast<uint64_t>(si->si_addr))) {
        gta6rt_isa_leave_host_to_signal(tok);
        return;
    }
    const uint64_t pc = sig == SIGTRAP ? mc->__ss.__rip - 1 : mc->__ss.__rip;
    ThreadRec* r = current_thread();
    bool handled = false;
    if (guest_code_contains(pc, r != nullptr)) {
        if (sig == SIGILL) handled = emulate_sse4a(mc) || emulate_zen(mc);
        if (sig == SIGTRAP) handled = emulate_trap_site(mc);
        if (!handled && sig == SIGILL) {
            if (gta6rt_illegal_insn_hook h = g_ill_hook.load()) {
                gta6rt_x64_context c;
                ctx_from_mcontext(mc, &c);
                if (h(&c, reinterpret_cast<const uint8_t*>(mc->__ss.__rip), g_ill_user) == GTA6RT_FAULT_RESUME) {
                    ctx_to_mcontext(&c, mc);
                    handled = true;
                }
            }
        }
        if (!handled) {
            if (gta6rt_guest_fault_handler h = g_fault_handler.load()) {
                gta6rt_x64_context c;
                gta6rt_x64_exception_record rec;
                memset(&rec, 0, sizeof rec);
                ctx_from_mcontext(mc, &c);
                if (sig == SIGTRAP) c.rip = pc;
                rec.code = exception_code(sig, si, reinterpret_cast<const uint8_t*>(mc->__ss.__rip), &rec, mc);
                rec.address = c.rip;
                if (h(&rec, &c, g_fault_user) == GTA6RT_FAULT_RESUME) {
                    ctx_to_mcontext(&c, mc);
                    handled = true;
                }
            }
        }
    }
    if (!handled) {
        gta6rt_isa_leave_host_to_signal(tok);
        chain(sig, si, ucv);
        return;
    }
    gta6rt_isa_leave_host_to_signal(tok);
}

void install_signals() {
    std::call_once(g_sig_once, [] {
        for (int sig : {SIGILL, SIGTRAP, SIGSEGV, SIGBUS, SIGFPE}) {
            struct sigaction sa;
            memset(&sa, 0, sizeof sa);
            sa.sa_sigaction = on_signal;
            sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
            sigemptyset(&sa.sa_mask);
            sigaction(sig, &sa, &g_old[sig]);
        }
    });
}

uint8_t* teb_from_gs() {
    const uint64_t t = gs_read(GTA6RT_TEB_SELF);
    return t ? reinterpret_cast<uint8_t*>(t) : nullptr;
}

}  // namespace

namespace gta6rt::isa {

gta6rt_status backend_init(const gta6rt_isa_config*) {
    int tr = 0;
    size_t n = sizeof tr;
    if (sysctlbyname("sysctl.proc_translated", &tr, &n, nullptr, 0) == 0) g_translated = tr == 1;
    set_zen_wait_provider(&rosetta_zen_wait_entry);
    install_signals();
    return GTA6RT_OK;
}

gta6rt_status backend_plant(ThreadRec* rec, gta6rt_teb_mode preferred) {
    const uint64_t s6 = gs_read(kSlotTeb), s11 = gs_read(kSlotTls), s12 = gs_read(kSlotPeb);
    if (s6 != 0) {
        log_line("thread_attach: TSD slot 6 is in use (0x%llx); neither SHARE nor SWAP can be used",
                 (unsigned long long)s6);
        return GTA6RT_E_BUSY;
    }
    const auto teb = reinterpret_cast<uint64_t>(rec->teb);
    if (preferred == GTA6RT_TEB_SHARE && s11 == 0 && s12 == 0) {
        gs_write(kSlotTeb, teb);
        gs_write(kSlotTls, teb_get(rec->teb, GTA6RT_TEB_TLS_POINTER));
        gs_write(kSlotPeb, teb_get(rec->teb, GTA6RT_TEB_PEB));
        rec->tsd_base = find_tsd_base(kSlotTeb);
        if (rec->tsd_base && rec->tsd_base[kSlotTeb / 8] != teb) rec->tsd_base = nullptr;
        if (!rec->tsd_base)
            log_line("thread_attach: TSD array of thread %u not found; TLS growth from other threads reaches its "
                     "gs:[0x58] only at its next host->guest call", rec->thread_id);
        rec->mode = GTA6RT_TEB_SHARE;
        return GTA6RT_OK;
    }
    if (preferred == GTA6RT_TEB_SHARE)
        log_line("thread_attach: TSD slot 11/12 in use (0x%llx, 0x%llx), thread falls back to SWAP",
                 (unsigned long long)s11, (unsigned long long)s12);
    uint64_t* tsd = find_tsd_base(kSlotTeb);
    if (!tsd) return GTA6RT_E_UNSUPPORTED;
    TebPrivate* p = teb_private(rec->teb);
    p->host_tsd = reinterpret_cast<uint64_t>(tsd);
    rec->tsd_base = tsd;
    retarget_native_thunks_to_entry();
    rec->mode = GTA6RT_TEB_SWAP;
    return GTA6RT_OK;
}

void backend_unplant(ThreadRec* rec) {
    if (rec->mode == GTA6RT_TEB_SHARE) {
        gs_write(kSlotTeb, 0);
        gs_write(kSlotTls, 0);
        gs_write(kSlotPeb, 0);
    }
}

void backend_publish(ThreadRec* rec, size_t teb_off, uint64_t v) {
    if (rec->mode != GTA6RT_TEB_SHARE) return;
    if (rec->tsd_base)
        __atomic_store_n(&rec->tsd_base[teb_off / 8], v, __ATOMIC_RELEASE);
    else if (rec == current_thread())
        gs_write(teb_off, v);
}

void backend_cpuid_raw(uint32_t leaf, uint32_t subleaf, gta6rt_cpuid_regs* o) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(leaf), "c"(subleaf));
    *o = {a, b, c, d};
}

uint64_t backend_xcr0_raw() {
    gta6rt_cpuid_regs r;
    backend_cpuid_raw(1, 0, &r);
    if (!(r.ecx & (1u << 27))) return 0;
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

}  // namespace gta6rt::isa

extern "C" {

__attribute__((ms_abi)) void gta6rt_rosetta_frame_dispatch(gta6rt_guest_frame* f, ThunkRec* t) {
    const uint64_t tok = gta6rt_isa_enter_host_from_signal();
    if (t->kind == GTA6RT_THUNK_TRAP) {
        log_signal_safe("[isa] unresolved import called: ");
        log_signal_safe(t->name);
        log_signal_safe("\n");
        if (!t->fn) abort();
    }
    reinterpret_cast<gta6rt_frame_handler>(const_cast<void*>(t->fn))(f, t->user);
    gta6rt_isa_leave_host_to_signal(tok);
}

gta6rt_isa_backend gta6rt_isa_backend_id(void) {
    return GTA6RT_ISA_ROSETTA;
}

const char* gta6rt_isa_backend_name(void) {
    return "rosetta";
}

uint64_t gta6rt_isa_advertised_features(void) {
    gta6rt_cpuid_regs l1, l7, e1;
    backend_cpuid_raw(1, 0, &l1);
    backend_cpuid_raw(7, 0, &l7);
    backend_cpuid_raw(0x80000001, 0, &e1);
    uint64_t f = 0;
    if (l1.ecx & (1u << 28)) f |= GTA6RT_CPUID_AVX;
    if (l1.ecx & (1u << 12)) f |= GTA6RT_CPUID_FMA3;
    if (l1.ecx & (1u << 29)) f |= GTA6RT_CPUID_F16C;
    if (l1.ecx & (1u << 22)) f |= GTA6RT_CPUID_MOVBE;
    if (l7.ebx & (1u << 3)) f |= GTA6RT_CPUID_BMI1;
    if (l7.ebx & (1u << 5)) f |= GTA6RT_CPUID_AVX2;
    if (l7.ebx & (1u << 8)) f |= GTA6RT_CPUID_BMI2;
    if (l7.ebx & (1u << 19)) f |= GTA6RT_CPUID_ADX;
    if (l7.ebx & (1u << 29)) f |= GTA6RT_CPUID_SHA;
    if (l7.ebx & (1u << 16)) f |= GTA6RT_CPUID_AVX512;
    if (l7.ecx & (1u << 9)) f |= GTA6RT_CPUID_VAES;
    if (e1.ecx & (1u << 5)) f |= GTA6RT_CPUID_LZCNT;
    if (e1.ecx & (1u << 6)) f |= GTA6RT_CPUID_SSE4A;
    if ((f & GTA6RT_CPUID_AVX) && (!(l1.ecx & (1u << 27)) || (backend_xcr0_raw() & 6) != 6)) f &= ~GTA6RT_CPUID_AVX;
    return f;
}

int gta6rt_isa_cpuid_enforced(void) {
    return cpuid_sites_trapped() || gta6rt_isa_advertised_features() == configured_features() ? 1 : 0;
}

uint64_t gta6rt_isa_host_features(void) {
    if (g_translated)
        return GTA6RT_CPUID_AVX | GTA6RT_CPUID_AVX2 | GTA6RT_CPUID_FMA3 | GTA6RT_CPUID_F16C | GTA6RT_CPUID_BMI1 |
               GTA6RT_CPUID_BMI2 | GTA6RT_CPUID_MOVBE | GTA6RT_CPUID_LZCNT;
    return gta6rt_isa_advertised_features();
}

gta6rt_status gta6rt_isa_reexec_for_cpuid(char** argv) {
    int tr = 0;
    size_t n = sizeof tr;
    if (sysctlbyname("sysctl.proc_translated", &tr, &n, nullptr, 0) != 0 || tr != 1) return GTA6RT_OK;
    const uint64_t want = configured_features();
    const uint64_t have = gta6rt_isa_advertised_features();
    if ((have & want) == want) return GTA6RT_OK;
    const char* again = getenv("GTA6RT_ISA_REEXECED");
    if (again && !strcmp(again, "1")) {
        log_line("ROSETTA_ADVERTISE_AVX=1 is set but Rosetta advertises 0x%llx of policy 0x%llx",
                 (unsigned long long)have, (unsigned long long)want);
        return GTA6RT_E_UNSUPPORTED;
    }
    if (!argv || !argv[0]) return GTA6RT_E_INVALID_ARG;
    char path[4096];
    uint32_t sz = sizeof path;
    if (_NSGetExecutablePath(path, &sz) != 0) return GTA6RT_E_NO_MEMORY;
    setenv("ROSETTA_ADVERTISE_AVX", "1", 1);
    setenv("GTA6RT_ISA_REEXECED", "1", 1);
    fflush(nullptr);
    execv(path, argv);
    log_line("re-exec of %s failed: %s", path, strerror(errno));
    return GTA6RT_E_IO;
}

uint64_t gta6rt_isa_enter_host_from_signal(void) {
    uint8_t* teb = teb_from_gs();
    if (!teb) return 0;
    TebPrivate* p = teb_private(teb);
    if (p->magic != kTebMagic || p->mode != GTA6RT_TEB_SWAP) return 0;
    set_tsd_base(p->host_tsd);
    return reinterpret_cast<uint64_t>(teb);
}

void gta6rt_isa_leave_host_to_signal(uint64_t token) {
    if (token) set_tsd_base(token);
}

gta6rt_status gta6rt_isa_call_guest(gta6rt_gva target, const gta6rt_guest_args* args, gta6rt_guest_ret* out) {
    if (!target || !args || args->count > 16) return GTA6RT_E_INVALID_ARG;
    gta6rt_status st = ensure_thread_attached();
    if (st != GTA6RT_OK) return st;
    ThreadRec* r = current_thread();
    gta6rt_guest_ret ret{};
    if (r->mode == GTA6RT_TEB_SWAP) {
        set_tsd_base(reinterpret_cast<uint64_t>(r->teb));
        gta6rt_rosetta_call_guest_asm(target, args->args, args->count, &ret);
        set_tsd_base(teb_private(r->teb)->host_tsd);
    } else {
        if (!r->tsd_base) gs_write(kSlotTls, teb_get(r->teb, GTA6RT_TEB_TLS_POINTER));
        gta6rt_rosetta_call_guest_asm(target, args->args, args->count, &ret);
    }
    if (out) *out = ret;
    return GTA6RT_OK;
}

uint64_t gta6rt_isa_call_guest4(gta6rt_gva target, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    gta6rt_guest_args a{};
    a.args[0] = a1;
    a.args[1] = a2;
    a.args[2] = a3;
    a.args[3] = a4;
    a.count = 4;
    gta6rt_guest_ret r{};
    if (gta6rt_isa_call_guest(target, &a, &r) != GTA6RT_OK) return 0;
    return r.rax;
}

gta6rt_status gta6rt_isa_run_entry(gta6rt_gva entry, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t* out_rax) {
    gta6rt_guest_args a{};
    a.args[0] = a1;
    a.args[1] = a2;
    a.args[2] = a3;
    a.count = 4;
    gta6rt_guest_ret r{};
    gta6rt_status st = gta6rt_isa_call_guest(entry, &a, &r);
    if (st == GTA6RT_OK && out_rax) *out_rax = r.rax;
    return st;
}

gta6rt_status gta6rt_isa_make_thunk(const gta6rt_thunk_desc* desc, gta6rt_gva* out) {
    if (!desc || !out) return GTA6RT_E_INVALID_ARG;
    if (desc->kind != GTA6RT_THUNK_NATIVE && desc->kind != GTA6RT_THUNK_FRAME && desc->kind != GTA6RT_THUNK_TRAP)
        return GTA6RT_E_INVALID_ARG;
    if (desc->kind != GTA6RT_THUNK_TRAP && !desc->fn) return GTA6RT_E_INVALID_ARG;
    auto* t = new ThunkRec{};
    t->fn = desc->fn;
    t->user = desc->user;
    t->kind = desc->kind;
    if (desc->debug_name) strlcpy(t->name, desc->debug_name, sizeof t->name);
    const uint64_t entry = desc->kind == GTA6RT_THUNK_NATIVE ? reinterpret_cast<uint64_t>(&gta6rt_rosetta_native_entry)
                                                             : reinterpret_cast<uint64_t>(&gta6rt_rosetta_frame_entry);
    uint8_t code[24] = {0x49, 0xBA, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0x25, 0, 0, 0, 0};
    const auto tp = reinterpret_cast<uint64_t>(t);
    memcpy(code + 2, &tp, 8);
    const uint64_t at = arena_reserve(entry, sizeof code, nullptr);
    if (!at || (at & 15)) {
        delete t;
        return GTA6RT_E_NO_MEMORY;
    }
    std::lock_guard<std::mutex> lk(g_thunk_mu);
    const bool direct = desc->kind == GTA6RT_THUNK_NATIVE && !g_entry_only && direct_thunks_env() &&
                        configured_teb_mode() == GTA6RT_TEB_SHARE;
    const uint64_t target = direct ? reinterpret_cast<uint64_t>(desc->fn) : entry;
    memcpy(code + 16, &target, 8);
    if (!arena_write(at, code, sizeof code)) {
        delete t;
        return GTA6RT_E_NO_MEMORY;
    }
    if (desc->kind == GTA6RT_THUNK_NATIVE) {
        if (!g_native_slots) g_native_slots = new std::vector<uint64_t>();
        g_native_slots->push_back(at + 16);
    }
    *out = at;
    return GTA6RT_OK;
}

gta6rt_status gta6rt_isa_make_native_thunk(const gta6rt_thunk_desc* desc, const gta6rt_native_sig* sig, gta6rt_gva* out) {
    (void)sig;
    if (desc && desc->kind != GTA6RT_THUNK_NATIVE) return GTA6RT_E_INVALID_ARG;
    return gta6rt_isa_make_thunk(desc, out);
}

gta6rt_status gta6rt_isa_capture_guest_context(gta6rt_x64_context* out) {
    (void)out;
    log_line("gta6rt_isa_capture_guest_context: not implemented on the rosetta backend (x86_64 seh captures with its "
             "own assembly)");
    return GTA6RT_E_UNSUPPORTED;
}

void gta6rt_isa_resume_guest_context(const gta6rt_x64_context* ctx) {
    (void)ctx;
    log_line("gta6rt_isa_resume_guest_context: not implemented on the rosetta backend; aborting");
    abort();
}

gta6rt_status gta6rt_isa_set_guest_fault_handler(gta6rt_guest_fault_handler fn, void* user) {
    g_fault_user = user;
    g_fault_handler.store(fn);
    install_signals();
    return GTA6RT_OK;
}

gta6rt_status gta6rt_isa_set_illegal_instruction_hook(gta6rt_illegal_insn_hook fn, void* user) {
    g_ill_user = user;
    g_ill_hook.store(fn);
    install_signals();
    return GTA6RT_OK;
}

}  // extern "C"
