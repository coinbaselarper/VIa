// isa/fexcore: arm64 host backend. Guest x86-64 code runs inside the vendored libFEXCore (third_party/fex) in this
// native arm64 process; nothing goes through Rosetta. The TEB is the guest GS base (FEX keeps it per guest thread),
// so no TSD slot is touched. Guest->host calls use FEX's thunk opcode (0F 3F) into gta6rt_fex_thunk_entry;
// host->guest calls use FEX's re-entrant JIT callback. Guest faults (FEX synchronous exceptions and host signals
// raised by JIT code) reach gta6rt_fex_exception_c with a Windows CONTEXT reconstructed from FEX's state.

#include "fexcore_internal.h"
#include "x86_patch.h"
#include "zen_insn.h"

#include <gta6rt/os.h>

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/CPUID.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/Thunks.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/IR/IR.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#include <FEXCore/Utils/JITWriteGuard.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/LongJump.h>

#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

using namespace gta6rt::isa;
using X = FEXCore::X86State::X86Reg;

extern "C" {
void gta6rt_fex_exception_entry(void);
[[noreturn]] void gta6rt_fex_resume(FEXCore::Core::CpuStateFrame* frame, uint64_t pc, uint64_t sp);
void gta6rt_fex_thunk_entry(void* rec);
gta6rt_status emit_thunk(void* rec, gta6rt_gva* out);
}

namespace gta6rt::fex {

namespace {

constexpr size_t kHostPage = 16384;
constexpr uint64_t kFatal = ~0ull;

fextl::unique_ptr<FEXCore::Context::Context> g_ctx;
FEXCore::SignalDelegatorConfig g_cfg{};
uint64_t g_cbret = 0;
FEXCore::Core::CPUState::gdt_segment g_gdt[32]{};
thread_local FexThread* t_ft = nullptr;

std::atomic<gta6rt_guest_fault_handler> g_fault_handler{nullptr};
void* g_fault_user = nullptr;
std::atomic<gta6rt_illegal_insn_hook> g_ill_hook{nullptr};
void* g_ill_user = nullptr;
struct sigaction g_old[32];
std::atomic<uint64_t> g_guest_syscalls{0};
std::atomic<uint64_t> g_unaligned_fixups{0};
std::atomic<uint64_t> g_callret_resets{0};

// FEX's 0F 3F thunk instruction carries a 32-byte id; every gta6rt thunk uses this one.
constexpr uint8_t kThunkId[32] = {'g', 't', 'a', '6', 'r', 't', '-', 'f', 'e', 'x', '-', 't', 'h', 'u', 'n', 'k',
                                  '-', 'v', '1', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0x5a};

struct ThunkRec {
    const void* fn;
    void* user;
    gta6rt_thunk_kind kind;
    NativeThunk* native;
    char name[96];
};

inline uint64_t read_fpcr() {
    uint64_t v;
    __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    return v;
}

inline void write_fpcr(uint64_t v) {
    __asm__ volatile("msr fpcr, %0" ::"r"(v));
}

// FEX keeps MXCSR in the guest state and its rounding mode and FTZ only in the hardware FPCR (FillSpecialRegs sets
// AH/NEP/FIZ, never RMode/FZ), so every entry into guest code derives them from MXCSR. x86 RC 01 (down) and 10 (up)
// are ARM RMode 10 (RM) and 01 (RP).
inline uint64_t guest_fpcr(const FexThread* ft) {
    const uint32_t mx = ft->frame->State.mxcsr;
    const uint64_t rc = (mx >> 13) & 3;
    const uint64_t rmode = ((rc & 1) << 1) | (rc >> 1);
    return (ft->host_fpcr & ~((3ull << 22) | (1ull << 24))) | (rmode << 22) | (uint64_t((mx >> 15) & 1) << 24);
}

inline void set_fpcr(uint64_t v) {
    if (read_fpcr() != v) write_fpcr(v);
}

int sysctl_int(const char* name) {
    int v = 0;
    size_t s = sizeof v;
    return sysctlbyname(name, &v, &s, nullptr, 0) == 0 ? v : 0;
}

bool arm_feat(const char* f) {
    char n[128];
    snprintf(n, sizeof n, "hw.optional.arm.%s", f);
    return sysctl_int(n) != 0;
}

// FEX's own detection reads Linux interfaces; this is the X6 spike's sysctl version.
FEXCore::HostFeatures darwin_host_features() {
    FEXCore::HostFeatures h{};
    const int line = sysctl_int("hw.cachelinesize");
    h.DCacheLineLog2 = line ? __builtin_ctz(uint32_t(line / 4)) : 4;
    h.SupportsCacheMaintenanceOps = true;
    h.SupportsAES = arm_feat("FEAT_AES");
    h.SupportsCRC = arm_feat("FEAT_CRC32");
    h.SupportsSHA = arm_feat("FEAT_SHA1") && arm_feat("FEAT_SHA256");
    h.SupportsAtomics = arm_feat("FEAT_LSE");
    h.SupportsRAND = arm_feat("FEAT_RNG");
    h.SupportsAFP = arm_feat("FEAT_AFP");
    h.SupportsRCPC = arm_feat("FEAT_LRCPC");
    h.SupportsTSOImm9 = arm_feat("FEAT_LRCPC2");
    h.SupportsPMULL_128Bit = arm_feat("FEAT_PMULL");
    h.SupportsCSSC = arm_feat("FEAT_CSSC");
    h.SupportsFCMA = arm_feat("FEAT_FCMA");
    h.SupportsFlagM = arm_feat("FEAT_FlagM");
    h.SupportsFlagM2 = arm_feat("FEAT_FlagM2");
    h.SupportsFRINTTS = arm_feat("FEAT_FRINTTS");
    h.SupportsRPRES = arm_feat("FEAT_RPRES");
    h.SupportsECV = arm_feat("FEAT_ECV");
    h.SupportsWFXT = arm_feat("FEAT_WFxT");
    h.SupportsI8MM = arm_feat("FEAT_I8MM");
    h.SupportsDotProd = arm_feat("FEAT_DotProd");
    h.SupportsBF16 = arm_feat("FEAT_BF16");
    h.SupportsMOPS = arm_feat("FEAT_MOPS");
    h.Supports3DNow = true;
    h.SupportsAVXInst = true;
    h.SupportsAES256 = h.SupportsAES;
    h.SupportsPreserveAllABI = true;
    h.SupportsCPUIndexInTPIDRRO = false;
    uint64_t dczid = 0;
    __asm__ volatile("mrs %0, dczid_el0" : "=r"(dczid));
    h.SupportsCLZERO = ((dczid >> 4) & 1) == 0 && (4u << (dczid & 0xF)) == 64;
    const uint64_t fpcr = read_fpcr();
    const uint64_t traps = (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 15);
    write_fpcr(fpcr | traps);
    const uint64_t probe = read_fpcr();
    write_fpcr(fpcr);
    h.SupportsFloatExceptions = (probe & traps) == traps;
    h.ProcessFeatures.HostType = FEXCore::HostFeatures::HostTypeEnum::Linux;
    h.ProcessFeatures.Is64BitMode = 1;
    const int ncpu = sysctl_int("hw.ncpu");
    h.CPUMIDRs.assign(ncpu > 0 ? ncpu : 1, 0x610F0000u);
    h.ProcessPID = uint32_t(getpid());
    return h;
}

class Delegator final : public FEXCore::SignalDelegator {
public:
    uintptr_t GetThunkCallbackRET() const override { return g_cbret; }
};

bool region_of(uint64_t a, uint64_t* base, uint64_t* size, bool* writable) {
    mach_vm_address_t addr = a;
    mach_vm_size_t sz = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &addr, &sz, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info),
                       &cnt, &obj) != KERN_SUCCESS)
        return false;
    if (addr > a || !(info.protection & VM_PROT_READ)) return false;
    *base = addr;
    *size = sz;
    *writable = (info.protection & VM_PROT_WRITE) != 0;
    return true;
}

class Syscalls final : public FEXCore::HLE::SyscallHandler {
public:
    void HandleSyscall(FEXCore::Core::CpuStateFrame* frame) override {
        auto& s = frame->State;
        if (g_guest_syscalls.fetch_add(1) < 16)
            log_line("guest executed syscall (rax=0x%llx) at rip 0x%llx: not supported, returns STATUS_NOT_IMPLEMENTED",
                     (unsigned long long)s.gregs[X::REG_RAX], (unsigned long long)s.rip);
        s.gregs[X::REG_RAX] = 0xC0000002u;
        s.rip += 2;
    }

    // Guest code runs from any readable mapping: the images, the isa arenas (thunks, stubs), test blobs.
    FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t a) override {
        uint64_t b = 0, sz = 0;
        bool w = false;
        if (!region_of(a, &b, &sz, &w)) return {0, 0, false};
        return {b, sz, w};
    }

    std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*,
                                                                                   uint64_t) override {
        return std::nullopt;
    }
};

class Thunks final : public FEXCore::ThunkHandler {
public:
    FEXCore::ThunkedFunction* LookupThunk(const FEXCore::IR::SHA256Sum& sum) override {
        if (memcmp(&sum, kThunkId, sizeof kThunkId) == 0) return &gta6rt_fex_thunk_entry;
        log_line("FEX asked for an unknown thunk id; guest code contains 0F 3F that gta6rt did not write");
        return nullptr;
    }
};

Delegator g_delegator;
Syscalls g_syscalls;
Thunks g_thunks;

FEXCore::CPUID::FunctionResults cpuid_entry(void*, uint32_t leaf, uint32_t subleaf) {
    gta6rt_cpuid_regs r{};
    gta6rt_isa_cpuid(leaf, subleaf, &r);
    return {r.eax, r.ebx, r.ecx, r.edx};
}

FEXCore::CPUID::XCRResults xcr_entry(void*, uint32_t fn) {
    if (fn != 0) return {0, 0};
    const uint64_t x = policy_xcr0(configured_features(), backend_xcr0_raw());
    return {static_cast<uint32_t>(x), static_cast<uint32_t>(x >> 32)};
}

void fex_msg(LogMan::DebugLevels level, const char* m) {
    if (level <= LogMan::INFO) fprintf(stderr, "[fex] %s\n", m);
}

void fex_assert(const char* m) {
    fprintf(stderr, "[fex] FATAL: %s\n", m);
    abort();
}

// ---- guest state <-> Windows CONTEXT --------------------------------------------------------------------

void state_to_context(FexThread* ft, gta6rt_x64_context* c) {
    const auto& s = ft->frame->State;
    memset(c, 0, sizeof *c);
    c->context_flags = 0x10000F;
    memcpy(&c->rax, s.gregs, 16 * sizeof(uint64_t));
    c->rip = s.rip;
    c->eflags = g_ctx->ReconstructCompactedEFLAGS(ft->thread, false, nullptr, 0);
    c->seg_cs = 0x33;
    c->seg_ss = c->seg_ds = c->seg_es = c->seg_gs = 0x2b;
    c->seg_fs = 0x53;
    c->mxcsr = s.mxcsr;
    uint8_t* fx = c->fp.flt_save;
    const uint16_t fsw = static_cast<uint16_t>((s.flags[FEXCore::X86State::X87FLAG_TOP_LOC] << 11) |
                                               (s.flags[FEXCore::X86State::X87FLAG_C0_LOC] << 8) |
                                               (s.flags[FEXCore::X86State::X87FLAG_C1_LOC] << 9) |
                                               (s.flags[FEXCore::X86State::X87FLAG_C2_LOC] << 10) |
                                               (s.flags[FEXCore::X86State::X87FLAG_C3_LOC] << 14) |
                                               s.flags[FEXCore::X86State::X87FLAG_IE_LOC]);
    memcpy(fx + 0, &s.FCW, 2);
    memcpy(fx + 2, &fsw, 2);
    fx[4] = s.AbridgedFTW;
    memcpy(fx + 24, &s.mxcsr, 4);
    const uint32_t mask = 0xFFFF;
    memcpy(fx + 28, &mask, 4);
    memcpy(fx + 32, s.mm, sizeof s.mm);
    for (int i = 0; i < 16; ++i) {
        c->fp.v.xmm[i].lo = s.xmm.sse.data[i][0];
        c->fp.v.xmm[i].hi = static_cast<int64_t>(s.xmm.sse.data[i][1]);
    }
    c->dr0 = s.Dr[0];
    c->dr1 = s.Dr[1];
    c->dr2 = s.Dr[2];
    c->dr3 = s.Dr[3];
    c->dr6 = s.Dr6;
    c->dr7 = s.Dr7;
}

void context_to_state(FexThread* ft, const gta6rt_x64_context* c) {
    auto& s = ft->frame->State;
    memcpy(s.gregs, &c->rax, 16 * sizeof(uint64_t));
    s.rip = c->rip;
    g_ctx->SetFlagsFromCompactedEFLAGS(ft->thread, c->eflags);
    s.mxcsr = c->mxcsr;
    memcpy(&s.FCW, c->fp.flt_save + 0, 2);
    s.AbridgedFTW = c->fp.flt_save[4];
    memcpy(s.mm, c->fp.flt_save + 32, sizeof s.mm);
    for (int i = 0; i < 16; ++i) {
        s.xmm.sse.data[i][0] = c->fp.v.xmm[i].lo;
        s.xmm.sse.data[i][1] = static_cast<uint64_t>(c->fp.v.xmm[i].hi);
    }
}

void record_from_sync_fault(FexThread* ft, gta6rt_x64_exception_record* r) {
    auto& f = ft->frame->SynchronousFaultData;
    auto& s = ft->frame->State;
    memset(r, 0, sizeof *r);
    r->address = s.rip;
    f.FaultToTopAndGeneratedException = false;
    switch (f.Signal) {
    case FEXCore::Core::FAULT_SIGILL:
        r->code = 0xC000001D;
        return;
    case FEXCore::Core::FAULT_SIGTRAP:
        if (f.TrapNo == FEXCore::X86State::X86_TRAPNO_DB) {
            r->code = 0x80000004;
            return;
        }
        if (f.TrapNo == FEXCore::X86State::X86_TRAPNO_BP) {
            s.rip -= 1;  // FEX leaves rip after the int3; Windows reports and resumes on the int3 itself
            r->address = s.rip;
            r->code = 0x80000003;
            r->n_params = 1;
            return;
        }
        break;
    case FEXCore::Core::FAULT_SIGSEGV:
        if (f.TrapNo == FEXCore::X86State::X86_TRAPNO_GP) {
            if ((f.err_code & 7) == 2) {
                switch (f.err_code >> 3) {
                case 3:
                    r->code = 0x80000003;
                    r->n_params = 1;
                    return;
                case 0x29:
                    r->code = 0xC0000409;
                    r->flags = 1;
                    r->n_params = 1;
                    r->params[0] = s.gregs[X::REG_RCX];
                    return;
                case 0x2c:
                    r->code = 0xC0000420;
                    return;
                case 0x2d:
                    s.rip += 3;
                    r->code = 0x80000003;
                    r->address = s.rip;
                    r->n_params = 1;
                    r->params[0] = s.gregs[X::REG_RAX];
                    return;
                default:
                    break;
                }
            } else {
                r->code = 0xC0000096;  // EXCEPTION_PRIV_INSTRUCTION
                return;
            }
        } else if (f.TrapNo == FEXCore::X86State::X86_TRAPNO_OF) {
            r->code = 0xC0000095;
            return;
        } else if (f.TrapNo == FEXCore::X86State::X86_TRAPNO_PF) {
            r->code = 0xC0000005;
            r->n_params = 2;
            r->params[0] = 8;
            r->params[1] = f.FaultAddress ? f.FaultAddress : s.rip;
            return;
        }
        break;
    default:
        break;
    }
    log_line("FEX synchronous fault signal %u trap %u err 0x%x at 0x%llx: reported as EXCEPTION_ILLEGAL_INSTRUCTION",
             f.Signal, f.TrapNo, f.err_code, (unsigned long long)s.rip);
    r->code = 0xC000001D;
}

void dump_context(const char* why, const gta6rt_x64_exception_record* r, const gta6rt_x64_context* c) {
    fprintf(stderr,
            "[isa] %s: exception 0x%08x at 0x%llx (params %u: 0x%llx 0x%llx)\n"
            "[isa]   rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n"
            "[isa]   rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n"
            "[isa]   r8 =%016llx r9 =%016llx r10=%016llx r11=%016llx\n"
            "[isa]   r12=%016llx r13=%016llx r14=%016llx r15=%016llx rip=%016llx efl=%08x\n",
            why, r->code, (unsigned long long)r->address, r->n_params, (unsigned long long)r->params[0],
            (unsigned long long)r->params[1], (unsigned long long)c->rax, (unsigned long long)c->rbx,
            (unsigned long long)c->rcx, (unsigned long long)c->rdx, (unsigned long long)c->rsi,
            (unsigned long long)c->rdi, (unsigned long long)c->rbp, (unsigned long long)c->rsp,
            (unsigned long long)c->r8, (unsigned long long)c->r9, (unsigned long long)c->r10,
            (unsigned long long)c->r11, (unsigned long long)c->r12, (unsigned long long)c->r13,
            (unsigned long long)c->r14, (unsigned long long)c->r15, (unsigned long long)c->rip, c->eflags);
}

// ---- host signals raised by JIT code ------------------------------------------------------------------

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

void reconstruct_from_mcontext(FexThread* ft, _STRUCT_MCONTEXT64* mc) {
    auto& s = ft->frame->State;
    s.rip = g_ctx->RestoreRIPFromHostPC(ft->thread, mc->__ss.__pc);
    for (size_t i = 0; i < g_cfg.SRAGPRCount; ++i) s.gregs[i] = mc->__ss.__x[g_cfg.SRAGPRMapping[i]];
    for (size_t i = 0; i < g_cfg.SRAFPRCount; ++i) memcpy(s.xmm.sse.data[i], &mc->__ns.__v[g_cfg.SRAFPRMapping[i]], 16);
    const uint32_t efl = g_ctx->ReconstructCompactedEFLAGS(ft->thread, true, mc->__ss.__x, mc->__ss.__cpsr);
    g_ctx->SetFlagsFromCompactedEFLAGS(ft->thread, efl);
}

// Darwin allocates a thread's thread_local block on first use (malloc), so the handler reads the TSD key instead.
FexThread* signal_thread() {
    ThreadRec* r = current_thread();
    return r ? static_cast<FexThread*>(r->backend) : nullptr;
}

void on_signal(int sig, siginfo_t* si, void* ucv) {
    auto* uc = static_cast<ucontext_t*>(ucv);
    _STRUCT_MCONTEXT64* mc = uc->uc_mcontext;
    FexThread* ft = signal_thread();
    const uint64_t pc = mc->__ss.__pc;
    if (!ft || !g_ctx || !g_ctx->IsAddressInCodeBuffer(ft->thread, pc)) {
        chain(sig, si, ucv);
        return;
    }
    const uint64_t addr = reinterpret_cast<uint64_t>(si->si_addr);
    const bool is_write = (mc->__es.__esr >> 6) & 1;
    if (sig == SIGSEGV || sig == SIGBUS) {
        const gta6rt_vm_fault_result vr = gta6rt_vm_handle_fault(si->si_addr, is_write ? 1 : 0);
        if (vr == GTA6RT_VM_FAULT_RETRY) return;
        auto* t = ft->thread;
        if (t->JITGuardPage && addr >= t->JITGuardPage && addr < t->JITGuardPage + FEXCore::Utils::HOST_GUARD_PAGE_SIZE) {
            FEXCore::UncheckedLongJump::ManuallyLoadJumpBuf(t->RestartJump, t->JITGuardOverflowArgument, &mc->__ss.__x[0],
                                                            reinterpret_cast<__uint128_t*>(&mc->__ns.__v[0]),
                                                            &mc->__ss.__pc);
            return;
        }
        const uint64_t cr = reinterpret_cast<uint64_t>(ft->callret);
        if (addr >= cr && addr < cr + ft->callret_size) {
            mc->__ss.__x[25] = reinterpret_cast<uint64_t>(t->CallRetStackBase) +
                               FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE / 4;
            g_callret_resets.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if ((mc->__es.__esr & 0x3f) == 0x21) {  // data abort, alignment fault
            FEXCore::Utils::JITWriteGuard writable;  // the handler may backpatch MAP_JIT code
            const auto fix = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
                t, FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier, pc, &mc->__ss.__x[0]);
            if (fix) {
                mc->__ss.__pc += *fix;
                g_unaligned_fixups.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        reconstruct_from_mcontext(ft, mc);
        gta6rt_x64_exception_record& r = ft->pending_rec;
        memset(&r, 0, sizeof r);
        r.address = ft->frame->State.rip;
        if (vr == GTA6RT_VM_FAULT_GUARD_PAGE) {
            r.code = 0x80000001;  // STATUS_GUARD_PAGE_VIOLATION
            r.n_params = 2;
            r.params[0] = is_write ? 1 : 0;
            r.params[1] = addr;
        } else if (addr >= ft->guest_floor - kHostPage && addr < ft->guest_floor) {
            r.code = 0xC00000FD;  // STATUS_STACK_OVERFLOW
            r.n_params = 2;
            r.params[0] = is_write ? 1 : 0;
            r.params[1] = addr;
        } else {
            r.code = 0xC0000005;
            r.n_params = 2;
            r.params[0] = is_write ? 1 : 0;
            r.params[1] = addr;
        }
    } else {
        log_signal_safe("[isa] FATAL: host SIGILL/SIGTRAP/SIGFPE inside FEX JIT code (FEX internal trap)\n");
        chain(sig, si, ucv);
        return;
    }
    ft->pending = true;
    mc->__ss.__x[1] = reinterpret_cast<uint64_t>(ft->frame);
    mc->__ss.__sp = ft->frame->ReturningStackLocation;
    mc->__ss.__pc = reinterpret_cast<uint64_t>(&gta6rt_fex_exception_entry);
}

std::once_flag g_sig_once;

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

bool is_host_code(gta6rt_gva fn) {
    if (guest_code_contains(fn, false)) return false;
    Dl_info info{};
    return dladdr(reinterpret_cast<void*>(fn), &info) != 0 && info.dli_fbase != nullptr;
}

uint64_t os_invoke(gta6rt_gva fn, const uint64_t* args, uint32_t count, void*) {
    if (is_host_code(fn)) {
        if (count > 8) {
            log_line("os invoker: host function 0x%llx called with %u args, only 8 integer args are passed",
                     (unsigned long long)fn, count);
            abort();
        }
        uint64_t a[8] = {};
        for (uint32_t i = 0; i < count; ++i) a[i] = args[i];
        using F = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
        return reinterpret_cast<F>(fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    }
    gta6rt_guest_args ga{};
    if (count > 16) {
        log_line("os invoker: guest function 0x%llx called with %u args (max 16)", (unsigned long long)fn, count);
        abort();
    }
    for (uint32_t i = 0; i < count; ++i) ga.args[i] = args[i];
    ga.count = count;
    gta6rt_guest_ret r{};
    const gta6rt_status st = gta6rt_isa_call_guest(fn, &ga, &r);
    if (st != GTA6RT_OK) {
        log_line("os invoker: call_guest(0x%llx) failed: %d", (unsigned long long)fn, st);
        abort();
    }
    return r.rax;
}

// Guest stack placement. A Windows thread's stack is the region the TEB describes; under FEX the host keeps using
// the same pthread stack while guest code runs, so the region is split: the host keeps the top part, a 16 KiB
// PROT_NONE page separates them, and the guest gets the rest down to the pthread stack limit. The TEB keeps the
// pthread bounds (what os/ and seh/ report). Host part: a quarter, 128 KiB to 2 MiB (boot to WinMain measured at most
// 64 KiB of host stack in use). Stacks under 512 KiB, or a thread already deeper than the split, get a separate guest
// stack of max(1 MiB, their size) and the TEB describes that one instead.
bool place_guest_stack(FexThread* ft, uint8_t* teb) {
    const uint64_t base = teb_get(teb, GTA6RT_TEB_STACK_BASE);
    const uint64_t limit = teb_get(teb, GTA6RT_TEB_STACK_LIMIT);
    uint64_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    const uint64_t total = base > limit ? base - limit : 0;
    const uint64_t host = std::clamp<uint64_t>(total / 4, 128u << 10, 2u << 20);
    const uint64_t split = (base - host) & ~(kHostPage - 1);
    const bool on_this_stack = sp > limit && sp <= base;
    if (total >= (512u << 10) && on_this_stack && sp > split + (64u << 10)) {
        const uint64_t guard = split - kHostPage;
        if (mprotect(reinterpret_cast<void*>(guard), kHostPage, PROT_NONE) == 0) {
            ft->split_guard = guard;
            ft->guest_top = guard;
            ft->guest_floor = limit;
            return true;
        }
        log_line("thread_attach: mprotect of the host/guest stack guard at 0x%llx failed: %s",
                 (unsigned long long)guard, strerror(errno));
    }
    const size_t sz = std::max<size_t>(1u << 20, (total + kHostPage - 1) & ~(kHostPage - 1));
    void* m = mmap(nullptr, sz + kHostPage, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) return false;
    auto* p = static_cast<uint8_t*>(m);
    if (mprotect(p + kHostPage, sz, PROT_READ | PROT_WRITE) != 0) {
        munmap(m, sz + kHostPage);
        return false;
    }
    ft->own_stack = p;
    ft->own_stack_size = sz + kHostPage;
    ft->guest_top = reinterpret_cast<uint64_t>(p) + kHostPage + sz;
    ft->guest_floor = reinterpret_cast<uint64_t>(p) + kHostPage;
    teb_put(teb, GTA6RT_TEB_STACK_BASE, ft->guest_top);
    teb_put(teb, GTA6RT_TEB_STACK_LIMIT, ft->guest_floor);
    log_line("thread_attach: thread %u has a %llu KiB host stack; guest stack is a separate %llu KiB mapping at 0x%llx",
             ft->rec->thread_id, (unsigned long long)(total >> 10), (unsigned long long)(sz >> 10),
             (unsigned long long)ft->guest_floor);
    return true;
}

}  // namespace

FEXCore::Context::Context* context() {
    return g_ctx.get();
}

FexThread* current() {
    return t_ft;
}

}  // namespace gta6rt::fex

using namespace gta6rt::fex;

// ---- backend hooks for common/ -------------------------------------------------------------------------------

namespace gta6rt::isa {

gta6rt_status backend_init(const gta6rt_isa_config*) {
    if (g_ctx) return GTA6RT_OK;
    LogMan::Throw::InstallHandler(fex_assert);
    LogMan::Msg::InstallHandler(fex_msg);
    FEXCore::Config::Initialize();
    FEXCore::Config::Load();
    FEXCore::Config::ReloadMetaLayer();
    FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
    const char* tso = getenv("GTA6RT_FEX_TSO");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_TSOENABLED, tso && !strcmp(tso, "0") ? "0" : "1");
    if (const char* mb = getenv("GTA6RT_FEX_MULTIBLOCK")) FEXCore::Config::Set(FEXCore::Config::CONFIG_MULTIBLOCK, mb);

    static const uint8_t kCbRet[16] = {0x0F, 0x3E, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
                                       0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    static int anchor;
    const uint64_t at = arena_reserve(reinterpret_cast<uint64_t>(&anchor), sizeof kCbRet, nullptr);
    if (!at || !arena_write(at, kCbRet, sizeof kCbRet)) {
        log_line("fexcore init: no arena for the callback return stub");
        return GTA6RT_E_NO_MEMORY;
    }
    g_cbret = at;

    auto hf = darwin_host_features();
    g_ctx = FEXCore::Context::Context::CreateNewContext(hf);
    g_ctx->SetSignalDelegator(&g_delegator);
    g_ctx->SetSyscallHandler(&g_syscalls);
    g_ctx->SetThunkHandler(&g_thunks);
    if (!g_ctx->InitCore()) {
        log_line("fexcore init: FEXCore InitCore failed");
        g_ctx.reset();
        return GTA6RT_E_UNSUPPORTED;
    }
    g_cfg = g_delegator.GetConfig();
    auto& cs = g_gdt[FEXCore::Core::CPUState::DEFAULT_USER_CS];
    FEXCore::Core::CPUState::SetGDTBase(&cs, 0);
    FEXCore::Core::CPUState::SetGDTLimit(&cs, 0xF'FFFFu);
    cs.L = 1;
    cs.D = 0;
    (void)current_thread();
    install_signals();
    gta6rt_os_set_invoker(&os_invoke, nullptr);
    return GTA6RT_OK;
}

gta6rt_status backend_plant(ThreadRec* rec, gta6rt_teb_mode) {
    if (!g_ctx) {
        log_line("thread_attach before gta6rt_isa_init: fexcore has no FEX context yet");
        return GTA6RT_E_UNSUPPORTED;
    }
    auto* ft = new FexThread;
    ft->rec = rec;
    ft->host_fpcr = read_fpcr();
    if (!place_guest_stack(ft, rec->teb)) {
        delete ft;
        return GTA6RT_E_NO_MEMORY;
    }
    const size_t crs = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
    void* cr = mmap(nullptr, crs + 2 * kHostPage, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (cr == MAP_FAILED || mprotect(static_cast<uint8_t*>(cr) + kHostPage, crs, PROT_READ | PROT_WRITE) != 0) {
        if (cr != MAP_FAILED) munmap(cr, crs + 2 * kHostPage);
        if (ft->split_guard) mprotect(reinterpret_cast<void*>(ft->split_guard), kHostPage, PROT_READ | PROT_WRITE);
        if (ft->own_stack) munmap(ft->own_stack, ft->own_stack_size);
        delete ft;
        return GTA6RT_E_NO_MEMORY;
    }
    ft->callret = static_cast<uint8_t*>(cr);
    ft->callret_size = crs + 2 * kHostPage;

    auto* t = g_ctx->CreateThread();
    ft->thread = t;
    ft->frame = t->CurrentFrame;
    auto& s = ft->frame->State;
    s.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = g_gdt;
    s.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = g_gdt;
    s.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
    s.cs_cached = FEXCore::Core::CPUState::CalculateGDTBase(g_gdt[FEXCore::Core::CPUState::DEFAULT_USER_CS]);
    s.gs_cached = reinterpret_cast<uint64_t>(rec->teb);
    s.mxcsr = 0x1F80;
    s.FCW = 0x27F;
    s.gregs[X::REG_RSP] = ft->guest_top - 0x40;
    t->CallRetStackBase = ft->callret + kHostPage;
    s.callret_sp = reinterpret_cast<uint64_t>(t->CallRetStackBase) + crs / 4;
    t->FrontendPtr = ft;
    auto& p = ft->frame->Pointers;
    p.ThunkCallbackRet = g_cbret;
    p.SynchronousExceptionHandler = reinterpret_cast<uint64_t>(&gta6rt_fex_exception_entry);
    p.CPUIDFunction = reinterpret_cast<uint64_t>(&cpuid_entry);
    p.XCRFunction = reinterpret_cast<uint64_t>(&xcr_entry);
    rec->backend = ft;
    rec->mode = GTA6RT_TEB_SWAP;  // the guest GS base is the TEB itself
    t_ft = ft;
    return GTA6RT_OK;
}

void backend_unplant(ThreadRec* rec) {
    auto* ft = static_cast<FexThread*>(rec->backend);
    if (!ft) return;
    if (ft->depth) log_line("thread_detach with %u host->guest calls active on thread %u", ft->depth, rec->thread_id);
    if (t_ft == ft) t_ft = nullptr;
    g_ctx->DestroyThread(ft->thread);
    munmap(ft->callret, ft->callret_size);
    if (ft->split_guard) mprotect(reinterpret_cast<void*>(ft->split_guard), kHostPage, PROT_READ | PROT_WRITE);
    if (ft->own_stack) munmap(ft->own_stack, ft->own_stack_size);
    rec->backend = nullptr;
    delete ft;
}

void backend_publish(ThreadRec*, size_t, uint64_t) {}

void backend_cpuid_raw(uint32_t leaf, uint32_t subleaf, gta6rt_cpuid_regs* o) {
    if (!g_ctx) {
        *o = {0, 0, 0, 0};
        if (leaf == 0) {
            o->eax = 0x16;
            memcpy(&o->ebx, "Genu", 4);
            memcpy(&o->edx, "ineI", 4);
            memcpy(&o->ecx, "ntel", 4);
        } else if (leaf == 1) {
            o->eax = 0x000906EA;
            o->ecx = 0x00982203;
            o->edx = 0x178BFBFF;
        } else if (leaf == 0x80000000) {
            o->eax = 0x80000008;
        } else if (leaf == 0x80000001) {
            o->ecx = 0x00000121;
            o->edx = 0x2C100800;
        }
        return;
    }
    const auto r = g_ctx->RunCPUIDFunction(leaf, subleaf);
    *o = {r.eax, r.ebx, r.ecx, r.edx};
}

uint64_t backend_xcr0_raw() {
    if (!g_ctx) return 0x7;
    const auto r = g_ctx->RunXCRFunction(0);
    return (static_cast<uint64_t>(r.edx) << 32) | r.eax;
}

}  // namespace gta6rt::isa

// ---- JIT entry points ----------------------------------------------------------------------------------------

namespace {

struct Resume {
    uint64_t pc;
    uint64_t sp;
};

uint64_t features_of(uint32_t l1c, uint32_t l7b, uint32_t l7c, uint32_t e1c) {
    uint64_t f = 0;
    if (l1c & (1u << 28)) f |= GTA6RT_CPUID_AVX;
    if (l1c & (1u << 12)) f |= GTA6RT_CPUID_FMA3;
    if (l1c & (1u << 29)) f |= GTA6RT_CPUID_F16C;
    if (l1c & (1u << 22)) f |= GTA6RT_CPUID_MOVBE;
    if (l7b & (1u << 3)) f |= GTA6RT_CPUID_BMI1;
    if (l7b & (1u << 5)) f |= GTA6RT_CPUID_AVX2;
    if (l7b & (1u << 8)) f |= GTA6RT_CPUID_BMI2;
    if (l7b & (1u << 19)) f |= GTA6RT_CPUID_ADX;
    if (l7b & (1u << 29)) f |= GTA6RT_CPUID_SHA;
    if (l7b & (1u << 16)) f |= GTA6RT_CPUID_AVX512;
    if (l7c & (1u << 9)) f |= GTA6RT_CPUID_VAES;
    if (e1c & (1u << 5)) f |= GTA6RT_CPUID_LZCNT;
    if (e1c & (1u << 6)) f |= GTA6RT_CPUID_SSE4A;
    return f;
}

}  // namespace

extern "C" {

Resume gta6rt_fex_exception_c(FEXCore::Core::CpuStateFrame* frame) {
    auto* ft = static_cast<FexThread*>(frame->Thread->FrontendPtr);
    set_fpcr(ft->host_fpcr);
    gta6rt_x64_exception_record rec;
    if (ft->pending) {
        rec = ft->pending_rec;
        ft->pending = false;
    } else {
        record_from_sync_fault(ft, &rec);
    }
    alignas(16) gta6rt_x64_context ctx;
    state_to_context(ft, &ctx);
    bool handled = false;
    if (rec.code == 0xC000001D || rec.code == 0xC0000096) {
        ZenDecoded z;
        const auto* ip = reinterpret_cast<const uint8_t*>(ctx.rip);
        if (zen_decode(ip, 15, &z)) {
            if (z.kind == ZenInsn::ClZero) {
                const uint64_t a = z.addr32 ? static_cast<uint32_t>(ctx.rax) : ctx.rax;
                memset(reinterpret_cast<void*>(a & ~uint64_t(63)), 0, 64);
            }
            ctx.rip += z.length;
            g_trap_count.fetch_add(1, std::memory_order_relaxed);
            handled = true;
        } else if (gta6rt_illegal_insn_hook h = g_ill_hook.load()) {
            handled = h(&ctx, ip, g_ill_user) == GTA6RT_FAULT_RESUME;
        }
    }
    if (!handled) {
        if (gta6rt_guest_fault_handler h = g_fault_handler.load()) {
            handled = h(&rec, &ctx, g_fault_user) == GTA6RT_FAULT_RESUME;
            if (!handled) dump_context("guest exception not handled by the registered handler", &rec, &ctx);
        } else {
            dump_context("guest exception and no handler registered (gta6rt_isa_set_guest_fault_handler)", &rec, &ctx);
        }
    }
    if (!handled) {
        fflush(stderr);
        abort();
    }
    if (ft->depth && ctx.rsp > ft->level_guest_rsp[ft->depth - 1]) {
        dump_context("FATAL: resume context is above the current host->guest call (non-local exit through host code)",
                     &rec, &ctx);
        abort();
    }
    context_to_state(ft, &ctx);
    set_fpcr(guest_fpcr(ft));
    return {g_cfg.AbsoluteLoopTopAddressFillSRA, frame->ReturningStackLocation};
}

void gta6rt_fex_thunk_entry(void* p) {
    auto* t = static_cast<ThunkRec*>(p);
    FexThread* ft = t_ft;
    auto& s = ft->frame->State;
    set_fpcr(ft->host_fpcr);
    s.gregs[X::REG_RDI] = s.gregs[X::REG_R11];
    ft->thunk_level[ft->thunk_depth++] = ft->depth;
    if (t->native) {
        if (!native_call(t->native, ft->frame)) {
            log_line("native thunk %s: stack arguments do not fit", t->name);
            abort();
        }
        --ft->thunk_depth;
        set_fpcr(guest_fpcr(ft));
        return;
    }
    gta6rt_guest_frame f;
    f.rcx = s.gregs[X::REG_RCX];
    f.rdx = s.gregs[X::REG_RDX];
    f.r8 = s.gregs[X::REG_R8];
    f.r9 = s.gregs[X::REG_R9];
    f.xmm0 = s.xmm.sse.data[0][0];
    f.xmm1 = s.xmm.sse.data[1][0];
    f.xmm2 = s.xmm.sse.data[2][0];
    f.xmm3 = s.xmm.sse.data[3][0];
    f.rsp = s.gregs[X::REG_RSP];
    f.rax = 0;
    f.xmm0_ret = 0;
    f.ret_is_float = 0;
    f.reserved = 0;
    if (t->kind == GTA6RT_THUNK_TRAP) {
        log_signal_safe("[isa] unresolved import called: ");
        log_signal_safe(t->name);
        log_signal_safe("\n");
        if (!t->fn) abort();
    }
    reinterpret_cast<gta6rt_frame_handler>(const_cast<void*>(t->fn))(&f, t->user);
    s.gregs[X::REG_RAX] = f.rax;
    if (f.ret_is_float) {
        s.xmm.sse.data[0][0] = f.xmm0_ret;
        s.xmm.sse.data[0][1] = 0;
    }
    --ft->thunk_depth;
    set_fpcr(guest_fpcr(ft));
}

static bool in_thunk_handler(const FexThread* ft) {
    return ft && ft->thunk_depth && ft->thunk_level[ft->thunk_depth - 1] == ft->depth;
}

gta6rt_status gta6rt_isa_capture_guest_context(gta6rt_x64_context* out) {
    FexThread* ft = t_ft;
    if (!out) return GTA6RT_E_INVALID_ARG;
    if (!in_thunk_handler(ft)) return GTA6RT_E_NOT_FOUND;
    state_to_context(ft, out);
    const uint64_t rsp = ft->frame->State.gregs[X::REG_RSP];
    memcpy(&out->rip, reinterpret_cast<const void*>(rsp), 8);
    out->rsp = rsp + 8;
    return GTA6RT_OK;
}

void gta6rt_isa_resume_guest_context(const gta6rt_x64_context* ctx) {
    FexThread* ft = t_ft;
    if (!in_thunk_handler(ft) || !ctx) {
        log_line("FATAL: gta6rt_isa_resume_guest_context outside a thunk handler (thread %u)", ft ? ft->rec->thread_id : 0);
        abort();
    }
    if (ft->depth && ctx->rsp > ft->level_guest_rsp[ft->depth - 1]) {
        gta6rt_x64_exception_record r{};
        r.address = ctx->rip;
        dump_context("FATAL: resume context is above the current host->guest call (non-local exit through host code)",
                     &r, ctx);
        abort();
    }
    context_to_state(ft, ctx);
    --ft->thunk_depth;
    set_fpcr(guest_fpcr(ft));
    gta6rt_fex_resume(ft->frame, g_cfg.AbsoluteLoopTopAddressFillSRA, ft->frame->ReturningStackLocation);
}

gta6rt_isa_backend gta6rt_isa_backend_id(void) {
    return GTA6RT_ISA_FEXCORE;
}

const char* gta6rt_isa_backend_name(void) {
    return "fexcore";
}

uint64_t gta6rt_isa_advertised_features(void) {
    gta6rt_cpuid_regs l1, l7, e1;
    gta6rt_isa_cpuid(1, 0, &l1);
    gta6rt_isa_cpuid(7, 0, &l7);
    gta6rt_isa_cpuid(0x80000001, 0, &e1);
    return features_of(l1.ecx, l7.ebx, l7.ecx, e1.ecx);
}

int gta6rt_isa_cpuid_enforced(void) {
    return g_ctx ? 1 : 0;
}

uint64_t gta6rt_isa_host_features(void) {
    gta6rt_cpuid_regs l1, l7, e1;
    backend_cpuid_raw(1, 0, &l1);
    backend_cpuid_raw(7, 0, &l7);
    backend_cpuid_raw(0x80000001, 0, &e1);
    return features_of(l1.ecx, l7.ebx, l7.ecx, e1.ecx);
}

gta6rt_status gta6rt_isa_reexec_for_cpuid(char**) {
    return GTA6RT_OK;
}

uint64_t gta6rt_isa_enter_host_from_signal(void) {
    return 0;
}

void gta6rt_isa_leave_host_to_signal(uint64_t) {}

gta6rt_status gta6rt_isa_call_guest(gta6rt_gva target, const gta6rt_guest_args* args, gta6rt_guest_ret* out) {
    if (!target || !args || args->count > 16) return GTA6RT_E_INVALID_ARG;
    if (!g_ctx) {
        log_line("call_guest before gta6rt_isa_init");
        return GTA6RT_E_UNSUPPORTED;
    }
    gta6rt_status st = ensure_thread_attached();
    if (st != GTA6RT_OK) return st;
    FexThread* ft = t_ft;
    if (!ft) return GTA6RT_E_NOT_FOUND;
    if (ft->depth >= 64) {
        log_line("call_guest: 64 nested host->guest calls on thread %u", ft->rec->thread_id);
        return GTA6RT_E_BUSY;
    }
    auto& s = ft->frame->State;
    const uint64_t saved_rsp = s.gregs[X::REG_RSP];
    const uint64_t saved_rip = s.rip;
    const uint64_t saved_level = ft->frame->ReturningStackLocation;
    const uint64_t from = ft->depth ? saved_rsp : ft->guest_top - 0x40;
    const uint64_t a = (from - 0x100 - 16 * 8) & ~uint64_t(15);
    for (uint32_t i = 4; i < args->count; ++i) reinterpret_cast<uint64_t*>(a + 0x20)[i - 4] = args->args[i];
    static constexpr X kRegs[4] = {X::REG_RCX, X::REG_RDX, X::REG_R8, X::REG_R9};
    for (uint32_t i = 0; i < 4 && i < args->count; ++i) {
        s.gregs[kRegs[i]] = args->args[i];
        if (args->float_mask & (1u << i)) {
            s.xmm.sse.data[i][0] = args->args[i];
            s.xmm.sse.data[i][1] = 0;
        }
    }
    s.gregs[X::REG_RSP] = a + 8;
    ft->level_guest_rsp[ft->depth] = a + 8;
    ++ft->depth;
    const uint64_t host_fpcr = read_fpcr();
    set_fpcr(guest_fpcr(ft));
    g_ctx->HandleCallback(ft->thread, target);
    set_fpcr(host_fpcr);
    --ft->depth;
    if (out) {
        out->rax = s.gregs[X::REG_RAX];
        out->xmm0_lo = s.xmm.sse.data[0][0];
    }
    s.gregs[X::REG_RSP] = saved_rsp;
    s.rip = saved_rip;
    ft->frame->ReturningStackLocation = saved_level;
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
    if (desc->kind == GTA6RT_THUNK_NATIVE) {
        log_line("make_thunk(%s): NATIVE thunks take a GTA6RT_GUEST_ABI (ms_abi) function, which arm64 does not have; "
                 "use FRAME, or gta6rt_isa_make_native_thunk with a signature",
                 desc->debug_name ? desc->debug_name : "?");
        return GTA6RT_E_UNSUPPORTED;
    }
    if (desc->kind != GTA6RT_THUNK_FRAME && desc->kind != GTA6RT_THUNK_TRAP) return GTA6RT_E_INVALID_ARG;
    if (desc->kind == GTA6RT_THUNK_FRAME && !desc->fn) return GTA6RT_E_INVALID_ARG;
    auto* t = new ThunkRec{};
    t->fn = desc->fn;
    t->user = desc->user;
    t->kind = desc->kind;
    if (desc->debug_name) strlcpy(t->name, desc->debug_name, sizeof t->name);
    return emit_thunk(t, out);
}

gta6rt_status gta6rt_isa_make_native_thunk(const gta6rt_thunk_desc* desc, const gta6rt_native_sig* sig, gta6rt_gva* out) {
    if (!desc || !sig || !out || !desc->fn || desc->kind != GTA6RT_THUNK_NATIVE || sig->count > 16)
        return GTA6RT_E_INVALID_ARG;
    for (unsigned i = 0; i < sig->count; ++i)
        if (sig->args[i].type == GTA6RT_ABI_VOID || sig->args[i].type > GTA6RT_ABI_AGG ||
            (sig->args[i].type == GTA6RT_ABI_AGG && (!sig->args[i].size || sig->args[i].size > 64)))
            return GTA6RT_E_INVALID_ARG;
    if (sig->ret.type > GTA6RT_ABI_AGG || (sig->ret.type == GTA6RT_ABI_AGG && (!sig->ret.size || sig->ret.size > 64)))
        return GTA6RT_E_INVALID_ARG;
    auto* t = new ThunkRec{};
    t->fn = desc->fn;
    t->user = desc->user;
    t->kind = GTA6RT_THUNK_NATIVE;
    t->native = new NativeThunk{desc->fn, *sig};
    if (desc->debug_name) strlcpy(t->name, desc->debug_name, sizeof t->name);
    return emit_thunk(t, out);
}

gta6rt_status emit_thunk(void* rec, gta6rt_gva* out) {
    auto* t = static_cast<ThunkRec*>(rec);
    uint8_t code[48];
    memset(code, 0xCC, sizeof code);
    code[0] = 0x4C;  // mov r11, rdi
    code[1] = 0x8B;
    code[2] = 0xDF;
    code[3] = 0x48;  // mov rdi, imm64
    code[4] = 0xBF;
    const auto tp = reinterpret_cast<uint64_t>(t);
    memcpy(code + 5, &tp, 8);
    code[13] = 0x0F;  // FEX thunk: calls gta6rt_fex_thunk_entry(rdi), then returns to the guest caller
    code[14] = 0x3F;
    memcpy(code + 15, kThunkId, sizeof kThunkId);
    static int anchor;
    const uint64_t at = arena_reserve(reinterpret_cast<uint64_t>(&anchor), sizeof code, nullptr);
    if (!at || !arena_write(at, code, sizeof code)) {
        delete t->native;
        delete t;
        return GTA6RT_E_NO_MEMORY;
    }
    *out = at;
    return GTA6RT_OK;
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
