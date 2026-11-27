// Guest exception entry for the FEX JIT (arm64). Entered either by FEX's dispatcher with a branch (synchronous
// fault: x0 = context, x1 = CpuStateFrame, sp = the callback level's stack pointer) or from our signal handler with
// the same register contract. The C++ side handles the exception and returns the frame and stack pointer to resume
// the JIT at AbsoluteLoopTopAddressFillSRA with the (possibly updated) guest state.

.text

.globl _gta6rt_fex_exception_entry
.p2align 4
_gta6rt_fex_exception_entry:
    mov x19, x1
    mov x29, #0
    mov x0, x1
    bl _gta6rt_fex_exception_c
    // x0 = resume pc, x1 = the level's stack pointer
    mov sp, x1
    mov x28, x19
    mov x16, x0
    mov x1, #0
    br x16

// uint64_t gta6rt_fex_native_call(const gta6rt_fex_native_regs* r)
//   r->x[8], r->v[8] (16 bytes each), r->x8, r->stack, r->stack_bytes (multiple of 16), r->fn
//   results written back: r->x[0], r->x[1], r->v[0..3]
.globl _gta6rt_fex_native_call
.p2align 4
_gta6rt_fex_native_call:
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    str x19, [sp, #16]
    mov x19, x0
    ldr x9, [x19, #200]
    sub sp, sp, x9
    ldr x10, [x19, #192]
    mov x11, #0
1:
    cmp x11, x9
    b.hs 2f
    ldr x12, [x10, x11]
    str x12, [sp, x11]
    add x11, x11, #8
    b 1b
2:
    ldp q0, q1, [x19, #64]
    ldp q2, q3, [x19, #96]
    ldp q4, q5, [x19, #128]
    ldp q6, q7, [x19, #160]
    ldr x8, [x19, #208]
    ldr x16, [x19, #216]
    ldp x0, x1, [x19, #0]
    ldp x2, x3, [x19, #16]
    ldp x4, x5, [x19, #32]
    ldp x6, x7, [x19, #48]
    blr x16
    stp x0, x1, [x19, #0]
    stp q0, q1, [x19, #64]
    stp q2, q3, [x19, #96]
    mov sp, x29
    ldr x19, [sp, #16]
    ldp x29, x30, [sp], #32
    ret

// void gta6rt_fex_resume(CpuStateFrame* frame, uint64_t pc, uint64_t sp): continue the JIT at pc with the callback
// level's stack pointer, the way gta6rt_fex_exception_entry resumes after a guest exception (no return).
.globl _gta6rt_fex_resume
.p2align 4
_gta6rt_fex_resume:
    mov sp, x2
    mov x28, x0
    mov x16, x1
    mov x29, #0
    mov x1, #0
    br x16
