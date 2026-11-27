#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include <gta6rt/isa.h>

#include "seh_internal.h"

#if defined(__x86_64__)

#include <pthread.h>
#include <sys/ucontext.h>

namespace seh {

struct fault_frame {
    ctx_t ctx;
    rec_t rec;
    alignas(16) uint8_t ymmh[16][16];
    uint32_t has_ymmh;
};

namespace {

bool from_ucontext(const ucontext_t* uc, ctx_t* c, uint8_t (*ymmh)[16], uint32_t* has_ymmh) {
    const auto* mc = uc->uc_mcontext;
    if (!mc) return false;
    memset(c, 0, sizeof *c);
    c->context_flags = kContextFull;
    const auto& s = mc->__ss;
    c->rax = s.__rax;
    c->rcx = s.__rcx;
    c->rdx = s.__rdx;
    c->rbx = s.__rbx;
    c->rsp = s.__rsp;
    c->rbp = s.__rbp;
    c->rsi = s.__rsi;
    c->rdi = s.__rdi;
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
    if (has_ymmh) *has_ymmh = 0;
    if (ymmh && uc->uc_mcsize >= sizeof(struct __darwin_mcontext_avx64)) {
        const auto* avx = reinterpret_cast<const struct __darwin_mcontext_avx64*>(mc);
        memcpy(ymmh, &avx->__fs.__fpu_ymmh0, 256);
        if (has_ymmh) *has_ymmh = 1;
    }
    return true;
}

bool to_ucontext(const ctx_t* c, ucontext_t* uc) {
    auto* mc = uc->uc_mcontext;
    if (!mc) return false;
    auto& s = mc->__ss;
    s.__rax = c->rax;
    s.__rcx = c->rcx;
    s.__rdx = c->rdx;
    s.__rbx = c->rbx;
    s.__rsp = c->rsp;
    s.__rbp = c->rbp;
    s.__rsi = c->rsi;
    s.__rdi = c->rdi;
    s.__r8 = c->r8;
    s.__r9 = c->r9;
    s.__r10 = c->r10;
    s.__r11 = c->r11;
    s.__r12 = c->r12;
    s.__r13 = c->r13;
    s.__r14 = c->r14;
    s.__r15 = c->r15;
    s.__rip = c->rip;
    s.__rflags = c->eflags;
    if (c->context_flags & 8) {
        memcpy(&mc->__fs.__fpu_fcw, c->fp.flt_save, 512);
        mc->__fs.__fpu_mxcsr = c->mxcsr;
    }
    return true;
}

uint32_t fpe_code(int si_code) {
    switch (si_code) {
    case FPE_INTDIV: return 0xC0000094u;
    case FPE_INTOVF: return 0xC0000095u;
    case FPE_FLTDIV: return 0xC000008Eu;
    case FPE_FLTOVF: return 0xC0000091u;
    case FPE_FLTUND: return 0xC0000093u;
    case FPE_FLTRES: return 0xC000008Fu;
    case FPE_FLTSUB: return 0xC000008Cu;
    default: return 0xC0000090u;
    }
}

void fill_record(int sig, const siginfo_t* si, const ucontext_t* uc, ctx_t* c, rec_t* r) {
    memset(r, 0, sizeof *r);
    r->address = c->rip;
    uint8_t insn[2] = {0, 0};
    safe_read(c->rip, insn, 2);
    const uint32_t err = uc && uc->uc_mcontext ? uc->uc_mcontext->__es.__err : 0;
    const uint32_t trapno = uc && uc->uc_mcontext ? uc->uc_mcontext->__es.__trapno : 0;
    const uint64_t addr = si ? reinterpret_cast<uint64_t>(si->si_addr) : 0;
    switch (sig) {
    case SIGSEGV:
    case SIGBUS:
        if (insn[0] == 0xcd && insn[1] == 0x29) {
            r->code = kStatusStackBufferOverrun;
            r->flags = kNonContinuable;
            r->n_params = 1;
            r->params[0] = c->rcx;
            break;
        }
        if (insn[0] == 0xcd && insn[1] == 0x2c) {
            r->code = kStatusAssertionFailure;
            c->rip += 2;
            break;
        }
        r->code = kStatusAccessViolation;
        r->n_params = 2;
        r->params[0] = (err & 2) ? 1 : ((err & 0x10) || addr == c->rip) ? 8 : 0;
        r->params[1] = trapno == 13 ? ~0ull : addr;
        break;
    case SIGILL:
        r->code = (si && si->si_code == ILL_PRVOPC) ? kStatusPrivInstruction : kStatusIllegalInstruction;
        break;
    case SIGFPE: r->code = fpe_code(si ? si->si_code : 0); break;
    case SIGTRAP: {
        uint8_t prev = 0;
        safe_read(c->rip - 1, &prev, 1);
        if (prev == 0xcc) {
            r->code = kStatusBreakpoint;
            c->rip -= 1;
            r->address = c->rip;
            r->n_params = 1;
        } else {
            r->code = kStatusSingleStep;
        }
        break;
    }
    default: r->code = kStatusIllegalInstruction; break;
    }
}

// Copies the fault onto the faulting thread's stack so dispatch runs after the signal handler returns
// (Windows KiUserExceptionDispatcher model). When the handler itself runs on that stack (no sigaltstack), the copy
// goes below the handler's live frames, which sigreturn discards. Returns the frame address or 0 if the stack has
// no room.
__attribute__((noinline)) uint64_t place_frame(const fault_frame* f) {
    pthread_t self = pthread_self();
    const uint64_t hi = reinterpret_cast<uint64_t>(pthread_get_stackaddr_np(self));
    const uint64_t lo = hi - pthread_get_stacksize_np(self);
    const uint64_t rsp = f->ctx.rsp;
    const bool on_thread_stack = rsp > lo && rsp <= hi;
    uint64_t cur;
    __asm__ volatile("movq %%rsp, %0" : "=r"(cur));
    uint64_t top = rsp - 256;
    if (cur < rsp && rsp - cur < (1ull << 20) && cur - 4096 < top) top = cur - 4096;
    const uint64_t sp = (top - sizeof(fault_frame)) & ~63ull;
    if (on_thread_stack && sp < lo + 32768) return 0;
    memcpy(reinterpret_cast<void*>(sp), f, sizeof *f);
    return sp;
}

[[noreturn]] void stack_overflow(fault_frame* f) {
    f->rec.code = kStatusStackOverflow;
    f->rec.flags = kNonContinuable;
    fatal_exception(&f->rec, &f->ctx, "stack overflow (no room to dispatch on the faulting stack)", 0);
}

gta6rt_fault_action isa_fault_handler(gta6rt_x64_exception_record* rec, gta6rt_x64_context* ctx, void*) {
    if (verbose())
        log("guest fault 0x%08x at 0x%llx rsp 0x%llx", rec->code, static_cast<unsigned long long>(ctx->rip),
            static_cast<unsigned long long>(ctx->rsp));
    if (!pc_in_guest_image(ctx->rip)) return GTA6RT_FAULT_CONTINUE_SEARCH;
    fault_frame f;
    memcpy(&f.ctx, ctx, sizeof f.ctx);
    memcpy(&f.rec, rec, sizeof f.rec);
    f.has_ymmh = 0;
    uint8_t insn[2] = {0, 0};
    safe_read(ctx->rip, insn, 2);
    if (insn[0] == 0xcd && insn[1] == 0x29) {
        memset(&f.rec, 0, sizeof f.rec);
        f.rec.code = kStatusStackBufferOverrun;
        f.rec.flags = kNonContinuable;
        f.rec.address = ctx->rip;
        f.rec.n_params = 1;
        f.rec.params[0] = ctx->rcx;
    }
    const uint64_t sp = place_frame(&f);
    if (!sp) stack_overflow(&f);
    ctx->rsp = sp;
    ctx->rip = reinterpret_cast<uint64_t>(&seh_fault_entry);
    ctx->rdi = sp;
    return GTA6RT_FAULT_RESUME;
}

}  // namespace

void register_isa_fault_handler() {
    const gta6rt_status s = gta6rt_isa_set_guest_fault_handler(isa_fault_handler, nullptr);
    if (s != GTA6RT_OK && verbose()) log("isa fault handler registration: status %d", s);
}

}  // namespace seh

extern "C" {

[[noreturn]] void seh_fault_dispatch_c(seh::fault_frame* f) {
    if (seh::dispatch_exception(&f->rec, &f->ctx)) {
        if (f->has_ymmh && __builtin_cpu_supports("avx")) {
            const uint32_t mx = f->ctx.mxcsr & 0xffffu;
            memcpy(f->ctx.fp.flt_save + 24, &mx, 4);
            seh_restore_raw_ymm(&f->ctx, &f->ymmh[0][0]);
        }
        seh::restore_context(&f->ctx, nullptr);
    }
    abort();
}

int gta6rt_seh_handle_signal(int sig, void* siginfo, void* ucontext) {
    auto* uc = static_cast<ucontext_t*>(ucontext);
    if (!uc || !uc->uc_mcontext) return 0;
    const uint64_t rip = uc->uc_mcontext->__ss.__rip;
    if (!seh::pc_in_guest_image(rip) && !(sig == SIGTRAP && seh::pc_in_guest_image(rip - 1))) return 0;
    seh::fault_frame f;
    if (!seh::from_ucontext(uc, &f.ctx, f.ymmh, &f.has_ymmh)) return 0;
    seh::fill_record(sig, static_cast<const siginfo_t*>(siginfo), uc, &f.ctx, &f.rec);
    const uint64_t sp = seh::place_frame(&f);
    if (!sp) seh::stack_overflow(&f);
    auto& s = uc->uc_mcontext->__ss;
    s.__rsp = sp;
    s.__rip = reinterpret_cast<uint64_t>(&seh_fault_entry);
    s.__rdi = sp;
    return 1;
}

gta6rt_status gta6rt_seh_context_from_ucontext(const void* ucontext, gta6rt_x64_context* ctx) {
    if (!ucontext || !ctx) return GTA6RT_E_INVALID_ARG;
    return seh::from_ucontext(static_cast<const ucontext_t*>(ucontext), ctx, nullptr, nullptr) ? GTA6RT_OK
                                                                                               : GTA6RT_E_INVALID_ARG;
}

gta6rt_status gta6rt_seh_context_to_ucontext(const gta6rt_x64_context* ctx, void* ucontext) {
    if (!ucontext || !ctx) return GTA6RT_E_INVALID_ARG;
    return seh::to_ucontext(ctx, static_cast<ucontext_t*>(ucontext)) ? GTA6RT_OK : GTA6RT_E_INVALID_ARG;
}

gta6rt_status gta6rt_seh_record_from_signal(int sig, const void* siginfo, const void* ucontext,
                                            gta6rt_x64_context* ctx, gta6rt_x64_exception_record* rec) {
    if (!ucontext || !ctx || !rec) return GTA6RT_E_INVALID_ARG;
    seh::fill_record(sig, static_cast<const siginfo_t*>(siginfo), static_cast<const ucontext_t*>(ucontext), ctx, rec);
    return GTA6RT_OK;
}
}

#else

namespace seh {
void register_isa_fault_handler() {}
}  // namespace seh

extern "C" {
int gta6rt_seh_handle_signal(int, void*, void*) { return 0; }
gta6rt_status gta6rt_seh_context_from_ucontext(const void*, gta6rt_x64_context*) { return GTA6RT_E_UNSUPPORTED; }
gta6rt_status gta6rt_seh_context_to_ucontext(const gta6rt_x64_context*, void*) { return GTA6RT_E_UNSUPPORTED; }
gta6rt_status gta6rt_seh_record_from_signal(int, const void*, const void*, gta6rt_x64_context*,
                                            gta6rt_x64_exception_record*) {
    return GTA6RT_E_UNSUPPORTED;
}
}

#endif
