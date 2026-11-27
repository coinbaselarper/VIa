#include "fault_guard.h"

#include <gta6rt/isa.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/ucontext.h>

extern "C" {
int d3d12vk_guarded_copy_asm(void* to, const void* from, std::size_t bytes);
int d3d12vk_guarded_compare_asm(const void* a, const void* b, std::size_t bytes);
int d3d12vk_guarded_gather_asm(void* to, const void* from, std::size_t count, std::size_t stride);
extern const char d3d12vk_guarded_copy_begin[], d3d12vk_guarded_copy_fault[];
extern const char d3d12vk_guarded_compare_begin[], d3d12vk_guarded_compare_fault[];
extern const char d3d12vk_guarded_gather_begin[], d3d12vk_guarded_gather_fault[];
}

#if defined(__x86_64__)
__asm__(R"(
    .text
    .p2align 4
    .globl _d3d12vk_guarded_copy_asm
    .globl _d3d12vk_guarded_copy_begin
    .globl _d3d12vk_guarded_copy_fault
_d3d12vk_guarded_copy_asm:
_d3d12vk_guarded_copy_begin:
    movq %rdx, %rcx
    cmpq $64, %rcx
    jb 2f
1:
    movdqu (%rsi), %xmm0
    movdqu 16(%rsi), %xmm1
    movdqu 32(%rsi), %xmm2
    movdqu 48(%rsi), %xmm3
    movdqu %xmm0, (%rdi)
    movdqu %xmm1, 16(%rdi)
    movdqu %xmm2, 32(%rdi)
    movdqu %xmm3, 48(%rdi)
    addq $64, %rsi
    addq $64, %rdi
    subq $64, %rcx
    cmpq $64, %rcx
    jae 1b
2:
    rep movsb
    xorl %eax, %eax
    ret
_d3d12vk_guarded_copy_fault:
    movl $1, %eax
    ret

    .p2align 4
    .globl _d3d12vk_guarded_compare_asm
    .globl _d3d12vk_guarded_compare_begin
    .globl _d3d12vk_guarded_compare_fault
_d3d12vk_guarded_compare_asm:
_d3d12vk_guarded_compare_begin:
    movq %rdx, %rcx
    cmpq $64, %rcx
    jb 3f
1:
    movdqu (%rdi), %xmm0
    movdqu 16(%rdi), %xmm1
    movdqu 32(%rdi), %xmm2
    movdqu 48(%rdi), %xmm3
    movdqu (%rsi), %xmm4
    movdqu 16(%rsi), %xmm5
    movdqu 32(%rsi), %xmm6
    movdqu 48(%rsi), %xmm7
    pcmpeqb %xmm4, %xmm0
    pcmpeqb %xmm5, %xmm1
    pcmpeqb %xmm6, %xmm2
    pcmpeqb %xmm7, %xmm3
    pand %xmm1, %xmm0
    pand %xmm3, %xmm2
    pand %xmm2, %xmm0
    pmovmskb %xmm0, %eax
    cmpl $0xffff, %eax
    jne 4f
    addq $64, %rdi
    addq $64, %rsi
    subq $64, %rcx
    cmpq $64, %rcx
    jae 1b
3:
    testq %rcx, %rcx
    jz 5f
    movb (%rdi), %al
    cmpb (%rsi), %al
    jne 4f
    incq %rdi
    incq %rsi
    decq %rcx
    jmp 3b
4:
    movl $1, %eax
    ret
5:
    xorl %eax, %eax
    ret
_d3d12vk_guarded_compare_fault:
    movl $2, %eax
    ret

    .p2align 4
    .globl _d3d12vk_guarded_gather_asm
    .globl _d3d12vk_guarded_gather_begin
    .globl _d3d12vk_guarded_gather_fault
_d3d12vk_guarded_gather_asm:
_d3d12vk_guarded_gather_begin:
    testq %rdx, %rdx
    jz 2f
1:
    movdqu (%rsi), %xmm0
    movdqu 16(%rsi), %xmm1
    movdqu 32(%rsi), %xmm2
    movdqu 48(%rsi), %xmm3
    movdqu %xmm0, (%rdi)
    movdqu %xmm1, 16(%rdi)
    movdqu %xmm2, 32(%rdi)
    movdqu %xmm3, 48(%rdi)
    addq $64, %rdi
    addq %rcx, %rsi
    decq %rdx
    jnz 1b
2:
    xorl %eax, %eax
    ret
_d3d12vk_guarded_gather_fault:
    movl $1, %eax
    ret
)");
#elif defined(__aarch64__)
__asm__(R"(
    .text
    .p2align 4
    .globl _d3d12vk_guarded_copy_asm
    .globl _d3d12vk_guarded_copy_begin
    .globl _d3d12vk_guarded_copy_fault
_d3d12vk_guarded_copy_asm:
_d3d12vk_guarded_copy_begin:
    cmp x2, #64
    b.lo 2f
1:
    ldp q0, q1, [x1]
    ldp q2, q3, [x1, #32]
    stp q0, q1, [x0]
    stp q2, q3, [x0, #32]
    add x1, x1, #64
    add x0, x0, #64
    sub x2, x2, #64
    cmp x2, #64
    b.hs 1b
2:
    cbz x2, 3f
    ldrb w3, [x1], #1
    strb w3, [x0], #1
    sub x2, x2, #1
    b 2b
3:
    mov w0, #0
    ret
_d3d12vk_guarded_copy_fault:
    mov w0, #1
    ret

    .p2align 4
    .globl _d3d12vk_guarded_compare_asm
    .globl _d3d12vk_guarded_compare_begin
    .globl _d3d12vk_guarded_compare_fault
_d3d12vk_guarded_compare_asm:
_d3d12vk_guarded_compare_begin:
    cmp x2, #16
    b.lo 3f
1:
    ldp x3, x4, [x0]
    ldp x5, x6, [x1]
    eor x3, x3, x5
    eor x4, x4, x6
    orr x3, x3, x4
    cbnz x3, 4f
    add x0, x0, #16
    add x1, x1, #16
    sub x2, x2, #16
    cmp x2, #16
    b.hs 1b
3:
    cbz x2, 5f
    ldrb w3, [x0], #1
    ldrb w4, [x1], #1
    cmp w3, w4
    b.ne 4f
    sub x2, x2, #1
    b 3b
4:
    mov w0, #1
    ret
5:
    mov w0, #0
    ret
_d3d12vk_guarded_compare_fault:
    mov w0, #2
    ret

    .p2align 4
    .globl _d3d12vk_guarded_gather_asm
    .globl _d3d12vk_guarded_gather_begin
    .globl _d3d12vk_guarded_gather_fault
_d3d12vk_guarded_gather_asm:
_d3d12vk_guarded_gather_begin:
    cbz x2, 2f
1:
    ldp q0, q1, [x1]
    ldp q2, q3, [x1, #32]
    stp q0, q1, [x0]
    stp q2, q3, [x0, #32]
    add x0, x0, #64
    add x1, x1, x3
    subs x2, x2, #1
    b.ne 1b
2:
    mov w0, #0
    ret
_d3d12vk_guarded_gather_fault:
    mov w0, #1
    ret
)");
#else
#error "fault_guard: no guarded copy for this architecture"
#endif

namespace rthost::fault_guard {
namespace {

#if defined(__x86_64__)
using HandlerAbi = __attribute__((ms_abi)) long (*)(void*);
#else
using HandlerAbi = long (*)(void*);
#endif

struct ExceptionRecord {
    std::uint32_t code;
    std::uint32_t flags;
    ExceptionRecord* chained;
    void* address;
    std::uint32_t parameters;
    std::uintptr_t information[15];
};
struct ExceptionPointers {
    ExceptionRecord* record;
    void* context;
};

constexpr std::size_t kMaxHandlers = 8;
std::atomic<VectoredHandler> g_handlers[kMaxHandlers];
std::atomic<unsigned> g_handler_count{0};
std::mutex g_handler_mutex;
struct sigaction g_old_segv, g_old_bus;
std::atomic<std::uint64_t> g_recovered{0};

bool veh_enabled() {
    static const bool on = [] {
        const char* v = std::getenv("LEONIDA_HOST_VEH");
        return v && v[0] == '1';
    }();
    return on;
}

std::uintptr_t pc_of(const ucontext_t* uc) {
#if defined(__x86_64__)
    return uc->uc_mcontext->__ss.__rip;
#else
    return reinterpret_cast<std::uintptr_t>(__darwin_arm_thread_state64_get_pc_fptr(uc->uc_mcontext->__ss));
#endif
}

void set_pc(ucontext_t* uc, const void* pc) {
#if defined(__x86_64__)
    uc->uc_mcontext->__ss.__rip = reinterpret_cast<std::uint64_t>(pc);
#else
    __darwin_arm_thread_state64_set_pc_fptr(uc->uc_mcontext->__ss, pc);
#endif
}

bool write_fault(const ucontext_t* uc) {
#if defined(__x86_64__)
    return (uc->uc_mcontext->__es.__err & 2) != 0;
#else
    return ((uc->uc_mcontext->__es.__esr >> 6) & 1) != 0;
#endif
}

void chain(int sig, siginfo_t* info, void* uc) {
    const struct sigaction& old = sig == SIGBUS ? g_old_bus : g_old_segv;
    if ((old.sa_flags & SA_SIGINFO) && old.sa_sigaction) {
        old.sa_sigaction(sig, info, uc);
        return;
    }
    if (old.sa_handler == SIG_IGN) return;
    if (old.sa_handler != SIG_DFL && old.sa_handler) {
        old.sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL);
}

bool in(std::uintptr_t pc, const char* begin, const char* end) {
    return pc >= reinterpret_cast<std::uintptr_t>(begin) && pc < reinterpret_cast<std::uintptr_t>(end);
}

void on_fault(int sig, siginfo_t* info, void* ucv) {
    auto* uc = static_cast<ucontext_t*>(ucv);
    const std::uintptr_t pc = pc_of(uc);
    if (in(pc, d3d12vk_guarded_copy_begin, d3d12vk_guarded_copy_fault)) {
        g_recovered.fetch_add(1, std::memory_order_relaxed);
        set_pc(uc, d3d12vk_guarded_copy_fault);
        return;
    }
    if (in(pc, d3d12vk_guarded_compare_begin, d3d12vk_guarded_compare_fault)) {
        g_recovered.fetch_add(1, std::memory_order_relaxed);
        set_pc(uc, d3d12vk_guarded_compare_fault);
        return;
    }
    if (in(pc, d3d12vk_guarded_gather_begin, d3d12vk_guarded_gather_fault)) {
        g_recovered.fetch_add(1, std::memory_order_relaxed);
        set_pc(uc, d3d12vk_guarded_gather_fault);
        return;
    }
    const unsigned count = g_handler_count.load(std::memory_order_acquire);
    if (count && info && write_fault(uc)) {
        ExceptionRecord record{};
        record.code = 0xC0000005u;
        record.address = reinterpret_cast<void*>(pc);
        record.parameters = 2;
        record.information[0] = 1;
        record.information[1] = reinterpret_cast<std::uintptr_t>(info->si_addr);
        ExceptionPointers pointers{&record, nullptr};
        const std::uint64_t token = gta6rt_isa_enter_host_from_signal();
        bool resumed = false;
        for (unsigned i = 0; i < count && !resumed; ++i)
            if (const VectoredHandler h = g_handlers[i].load(std::memory_order_acquire))
                resumed = reinterpret_cast<HandlerAbi>(h)(&pointers) == -1;
        gta6rt_isa_leave_host_to_signal(token);
        if (resumed) return;
    }
    chain(sig, info, ucv);
}

}  // namespace

bool install() {
    static const bool ok = [] {
        struct sigaction sa{};
        sa.sa_sigaction = on_fault;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        const bool done = sigaction(SIGSEGV, &sa, &g_old_segv) == 0 && sigaction(SIGBUS, &sa, &g_old_bus) == 0;
        std::fprintf(stderr, "[rt] fault_guard: %s (fault-safe copies of title memory%s)\n",
                     done ? "SIGSEGV/SIGBUS handler chained in front of the runtime's" : "sigaction failed",
                     veh_enabled() ? "; the layer's vectored handlers get write faults, LEONIDA_HOST_VEH=1" : "");
        return done;
    }();
    return ok;
}

bool copy(void* to, const void* from, std::size_t bytes) {
    if (!bytes) return true;
    if (!install()) return false;
    return d3d12vk_guarded_copy_asm(to, from, bytes) == 0;
}

int compare(const void* a, const void* b, std::size_t bytes) {
    if (!bytes) return 0;
    if (!install()) return -1;
    const int r = d3d12vk_guarded_compare_asm(a, b, bytes);
    return r == 2 ? -1 : r;
}

bool gather(void* to, const void* from, std::size_t count, std::size_t stride) {
    if (!count) return true;
    if (!install()) return false;
    return d3d12vk_guarded_gather_asm(to, from, count, stride) == 0;
}

std::uint64_t recovered() { return g_recovered.load(std::memory_order_relaxed); }

bool add_vectored_handler(bool first, VectoredHandler handler) {
    if (!handler || !veh_enabled() || !install()) return false;
    std::lock_guard lock(g_handler_mutex);
    const unsigned count = g_handler_count.load(std::memory_order_relaxed);
    if (count >= kMaxHandlers) return false;
    if (first) {
        for (unsigned i = count; i > 0; --i)
            g_handlers[i].store(g_handlers[i - 1].load(std::memory_order_relaxed), std::memory_order_release);
        g_handlers[0].store(handler, std::memory_order_release);
    } else {
        g_handlers[count].store(handler, std::memory_order_release);
    }
    g_handler_count.store(count + 1, std::memory_order_release);
    return true;
}

bool remove_vectored_handler(VectoredHandler handler) {
    std::lock_guard lock(g_handler_mutex);
    const unsigned count = g_handler_count.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < count; ++i) {
        if (g_handlers[i].load(std::memory_order_relaxed) != handler) continue;
        for (unsigned j = i; j + 1 < count; ++j)
            g_handlers[j].store(g_handlers[j + 1].load(std::memory_order_relaxed), std::memory_order_release);
        g_handlers[count - 1].store(nullptr, std::memory_order_release);
        g_handler_count.store(count - 1, std::memory_order_release);
        return true;
    }
    return false;
}

}  // namespace rthost::fault_guard
