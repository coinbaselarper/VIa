// x86_64 Mach-O. Guest-callable entry points (RtlCaptureContext, RaiseException, ...) use the Windows x64
// convention; seh_* helpers called from C use SysV. No .subsections_via_symbols: the blob's label differences
// must stay assembly-time constants.

#define CTX_FLAGS  0x30
#define CTX_MXCSR  0x34
#define CTX_SEGCS  0x38
#define CTX_SEGDS  0x3a
#define CTX_SEGES  0x3c
#define CTX_SEGFS  0x3e
#define CTX_SEGGS  0x40
#define CTX_SEGSS  0x42
#define CTX_EFLAGS 0x44
#define CTX_DR0    0x48
#define CTX_RAX    0x78
#define CTX_RCX    0x80
#define CTX_RDX    0x88
#define CTX_RBX    0x90
#define CTX_RSP    0x98
#define CTX_RBP    0xa0
#define CTX_RSI    0xa8
#define CTX_RDI    0xb0
#define CTX_R8     0xb8
#define CTX_R9     0xc0
#define CTX_R10    0xc8
#define CTX_R11    0xd0
#define CTX_R12    0xd8
#define CTX_R13    0xe0
#define CTX_R14    0xe8
#define CTX_R15    0xf0
#define CTX_RIP    0xf8
#define CTX_FLT    0x100
#define CTX_XMM    0x1a0

// Trampoline frame (body rsp B): [B+0x00] shadow space, [B+0x20] tramp_args*, [B+0x28..0x68) rbx rbp rsi rdi
// r12-r15 copied from tramp_args.frame_ctx, [B+0x70..0x110) xmm6-15, [B+0x110..0x138) machine frame.
#define TF_ARGS  0x20
#define TF_GPR   0x28
#define TF_XMM   0x70
#define TF_BODY  0x110
#define TF_MF    0x110

#define STUB_FRAME 0x518
#define STUB_CTX   0x20

    .text

// ---- blob: code + UNWIND_INFO + RUNTIME_FUNCTIONs registered with seh's own table at init ----------------

    .globl _seh_blob_base
    .p2align 4
_seh_blob_base:

.macro TRAMP name
    .globl \name
    .p2align 4
\name:
    subq $(0x28), %rsp
L\name\()_mf:
    subq $(TF_BODY), %rsp
L\name\()_pe:
    movq %rdi, TF_ARGS(%rsp)
    movq 0x28(%rdi), %rax
    movq CTX_RIP(%rax), %rcx
    movq %rcx, TF_MF+0x00(%rsp)
    movzwl CTX_SEGCS(%rax), %ecx
    movq %rcx, TF_MF+0x08(%rsp)
    movl CTX_EFLAGS(%rax), %ecx
    movq %rcx, TF_MF+0x10(%rsp)
    movq CTX_RSP(%rax), %rcx
    movq %rcx, TF_MF+0x18(%rsp)
    movzwl CTX_SEGSS(%rax), %ecx
    movq %rcx, TF_MF+0x20(%rsp)
    movq CTX_RBX(%rax), %rcx
    movq %rcx, TF_GPR+0x00(%rsp)
    movq CTX_RBP(%rax), %rcx
    movq %rcx, TF_GPR+0x08(%rsp)
    movq CTX_RSI(%rax), %rcx
    movq %rcx, TF_GPR+0x10(%rsp)
    movq CTX_RDI(%rax), %rcx
    movq %rcx, TF_GPR+0x18(%rsp)
    movq CTX_R12(%rax), %rcx
    movq %rcx, TF_GPR+0x20(%rsp)
    movq CTX_R13(%rax), %rcx
    movq %rcx, TF_GPR+0x28(%rsp)
    movq CTX_R14(%rax), %rcx
    movq %rcx, TF_GPR+0x30(%rsp)
    movq CTX_R15(%rax), %rcx
    movq %rcx, TF_GPR+0x38(%rsp)
    .irp r, 6,7,8,9,10,11,12,13,14,15
    movups CTX_XMM+\r*16(%rax), %xmm0
    movaps %xmm0, TF_XMM+(\r-6)*16(%rsp)
    .endr
    movq 0x08(%rdi), %rcx
    movq 0x10(%rdi), %rdx
    movq 0x18(%rdi), %r8
    movq 0x20(%rdi), %r9
    call *(%rdi)
    nop
    addq $(TF_BODY+0x28), %rsp
    ret
L\name\()_end:
.endm

.macro SAVE_GPR_CODE name, reg, slot
    .byte L\name\()_pe - \name
    .byte 4 | (\reg << 4)
    .short (TF_GPR + \slot * 8) / 8
.endm

.macro TRAMP_UNWIND name, flags, handler
    .p2align 2
L\name\()_uw:
    .byte 1 | (\flags << 3)
    .byte L\name\()_pe - \name
    .byte 39
    .byte 0
    .irp r, 15,14,13,12,11,10,9,8,7,6
    .byte L\name\()_pe - \name
    .byte 8 | (\r << 4)
    .short (TF_XMM + (\r - 6) * 16) / 16
    .endr
    SAVE_GPR_CODE \name, 15, 7
    SAVE_GPR_CODE \name, 14, 6
    SAVE_GPR_CODE \name, 13, 5
    SAVE_GPR_CODE \name, 12, 4
    SAVE_GPR_CODE \name, 7, 3
    SAVE_GPR_CODE \name, 6, 2
    SAVE_GPR_CODE \name, 5, 1
    SAVE_GPR_CODE \name, 3, 0
    .byte L\name\()_pe - \name
    .byte 1
    .short TF_BODY / 8
    .byte L\name\()_mf - \name
    .byte 10
    .short 0
    .if \flags
    .long \handler - _seh_blob_base
    .long 0
    .endif
.endm

    TRAMP _seh_tramp_plain
    TRAMP _seh_tramp_exception
    TRAMP _seh_tramp_unwind
    TRAMP _seh_tramp_consolidate

    .p2align 4
Lseh_nested_stub:
    jmp _seh_nested_handler_c
    .p2align 4
Lseh_collided_stub:
    jmp _seh_collided_handler_c

    TRAMP_UNWIND _seh_tramp_plain, 0, 0
    TRAMP_UNWIND _seh_tramp_exception, 3, Lseh_nested_stub
    TRAMP_UNWIND _seh_tramp_unwind, 3, Lseh_collided_stub
    TRAMP_UNWIND _seh_tramp_consolidate, 0, 0

    .p2align 2
    .globl _seh_blob_pdata
_seh_blob_pdata:
    .long _seh_tramp_plain - _seh_blob_base, L_seh_tramp_plain_end - _seh_blob_base, L_seh_tramp_plain_uw - _seh_blob_base
    .long _seh_tramp_exception - _seh_blob_base, L_seh_tramp_exception_end - _seh_blob_base, L_seh_tramp_exception_uw - _seh_blob_base
    .long _seh_tramp_unwind - _seh_blob_base, L_seh_tramp_unwind_end - _seh_blob_base, L_seh_tramp_unwind_uw - _seh_blob_base
    .long _seh_tramp_consolidate - _seh_blob_base, L_seh_tramp_consolidate_end - _seh_blob_base, L_seh_tramp_consolidate_uw - _seh_blob_base
    .globl _seh_blob_pdata_count
_seh_blob_pdata_count:
    .long 4

    .globl _seh_blob_end
_seh_blob_end:

// ---- context capture / restore --------------------------------------------------------------------------

// void RtlCaptureContext(CONTEXT* rcx): the caller's state at the return address.
    .globl _RtlCaptureContext
    .p2align 4
_RtlCaptureContext:
    movq %rax, CTX_RAX(%rcx)
    pushfq
    popq %rax
    movl %eax, CTX_EFLAGS(%rcx)
    movl $0x10000f, CTX_FLAGS(%rcx)
    stmxcsr CTX_MXCSR(%rcx)
    movw %cs, CTX_SEGCS(%rcx)
    movw %ds, CTX_SEGDS(%rcx)
    movw %es, CTX_SEGES(%rcx)
    movw %fs, CTX_SEGFS(%rcx)
    movw %gs, CTX_SEGGS(%rcx)
    movw %ss, CTX_SEGSS(%rcx)
    movq %rcx, CTX_RCX(%rcx)
    movq %rdx, CTX_RDX(%rcx)
    movq %rbx, CTX_RBX(%rcx)
    leaq 8(%rsp), %rax
    movq %rax, CTX_RSP(%rcx)
    movq %rbp, CTX_RBP(%rcx)
    movq %rsi, CTX_RSI(%rcx)
    movq %rdi, CTX_RDI(%rcx)
    movq %r8, CTX_R8(%rcx)
    movq %r9, CTX_R9(%rcx)
    movq %r10, CTX_R10(%rcx)
    movq %r11, CTX_R11(%rcx)
    movq %r12, CTX_R12(%rcx)
    movq %r13, CTX_R13(%rcx)
    movq %r14, CTX_R14(%rcx)
    movq %r15, CTX_R15(%rcx)
    movq (%rsp), %rax
    movq %rax, CTX_RIP(%rcx)
    fxsave CTX_FLT(%rcx)
    movq CTX_RAX(%rcx), %rax
    ret

.macro RESTORE_GPRS_AND_JUMP
    movq CTX_RSP(%rcx), %r11
    movq CTX_RIP(%rcx), %rax
    movq %rax, -8(%r11)
    movq CTX_RCX(%rcx), %rax
    movq %rax, -16(%r11)
    movl CTX_EFLAGS(%rcx), %eax
    movq %rax, -24(%r11)
    movq CTX_R11(%rcx), %rax
    movq %rax, -32(%r11)
    movq CTX_RDX(%rcx), %rdx
    movq CTX_RBX(%rcx), %rbx
    movq CTX_RBP(%rcx), %rbp
    movq CTX_RSI(%rcx), %rsi
    movq CTX_RDI(%rcx), %rdi
    movq CTX_R8(%rcx), %r8
    movq CTX_R9(%rcx), %r9
    movq CTX_R10(%rcx), %r10
    movq CTX_R12(%rcx), %r12
    movq CTX_R13(%rcx), %r13
    movq CTX_R14(%rcx), %r14
    movq CTX_R15(%rcx), %r15
    movq CTX_RAX(%rcx), %rax
    leaq -32(%r11), %rsp
    popq %r11
    popfq
    popq %rcx
    ret
.endm

// [[noreturn]] void seh_restore_raw(const CONTEXT* rdi)
    .globl _seh_restore_raw
    .p2align 4
_seh_restore_raw:
    movq %rdi, %rcx
    testl $8, CTX_FLAGS(%rcx)
    jz 1f
    fxrstor CTX_FLT(%rcx)
1:
    RESTORE_GPRS_AND_JUMP

// [[noreturn]] void seh_restore_raw_ymm(const CONTEXT* rdi, const uint8_t ymmh[16][16] rsi)
    .globl _seh_restore_raw_ymm
    .p2align 4
_seh_restore_raw_ymm:
    movq %rdi, %rcx
    fxrstor CTX_FLT(%rcx)
    .irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    vinsertf128 $(1), \r*16(%rsi), %ymm\r, %ymm\r
    .endr
    RESTORE_GPRS_AND_JUMP

// ---- guest-callable entry stubs that need the caller's context --------------------------------------------

.macro STUB_CAPTURE
    subq $(STUB_FRAME), %rsp
    movq %rax, STUB_CTX+CTX_RAX(%rsp)
    movq %rcx, STUB_CTX+CTX_RCX(%rsp)
    movq %rdx, STUB_CTX+CTX_RDX(%rsp)
    movq %rbx, STUB_CTX+CTX_RBX(%rsp)
    movq %rbp, STUB_CTX+CTX_RBP(%rsp)
    movq %rsi, STUB_CTX+CTX_RSI(%rsp)
    movq %rdi, STUB_CTX+CTX_RDI(%rsp)
    movq %r8, STUB_CTX+CTX_R8(%rsp)
    movq %r9, STUB_CTX+CTX_R9(%rsp)
    movq %r10, STUB_CTX+CTX_R10(%rsp)
    movq %r11, STUB_CTX+CTX_R11(%rsp)
    movq %r12, STUB_CTX+CTX_R12(%rsp)
    movq %r13, STUB_CTX+CTX_R13(%rsp)
    movq %r14, STUB_CTX+CTX_R14(%rsp)
    movq %r15, STUB_CTX+CTX_R15(%rsp)
    leaq STUB_FRAME+8(%rsp), %rax
    movq %rax, STUB_CTX+CTX_RSP(%rsp)
    movq STUB_FRAME(%rsp), %rax
    movq %rax, STUB_CTX+CTX_RIP(%rsp)
    pushfq
    popq %rax
    movl %eax, STUB_CTX+CTX_EFLAGS(%rsp)
    movl $(0x10000f), STUB_CTX+CTX_FLAGS(%rsp)
    stmxcsr STUB_CTX+CTX_MXCSR(%rsp)
    fxsave STUB_CTX+CTX_FLT(%rsp)
    movw %cs, STUB_CTX+CTX_SEGCS(%rsp)
    movw %ds, STUB_CTX+CTX_SEGDS(%rsp)
    movw %es, STUB_CTX+CTX_SEGES(%rsp)
    movw %fs, STUB_CTX+CTX_SEGFS(%rsp)
    movw %gs, STUB_CTX+CTX_SEGGS(%rsp)
    movw %ss, STUB_CTX+CTX_SEGSS(%rsp)
    xorl %eax, %eax
    .irp off, 0x00,0x08,0x10,0x18,0x20,0x28,0x48,0x50,0x58,0x60,0x68,0x70
    movq %rax, STUB_CTX+\off(%rsp)
    .endr
    movq STUB_CTX+CTX_RAX(%rsp), %rax
.endm

// void RaiseException(DWORD code, DWORD flags, DWORD count, const ULONG_PTR* args)
    .globl _RaiseException
    .p2align 4
_RaiseException:
    STUB_CAPTURE
    movl %ecx, %edi
    movl %edx, %esi
    movq %r9, %rcx
    movl %r8d, %edx
    leaq STUB_FRAME(%rsp), %r8
    leaq STUB_CTX(%rsp), %r9
    call _seh_raise_entry
    leaq STUB_CTX(%rsp), %rdi
    jmp _seh_restore_raw

// void RtlRaiseException(EXCEPTION_RECORD* rec)
    .globl _RtlRaiseException
    .p2align 4
_RtlRaiseException:
    STUB_CAPTURE
    movq %rcx, %rdi
    leaq STUB_CTX(%rsp), %rsi
    call _seh_rtl_raise_entry
    leaq STUB_CTX(%rsp), %rdi
    jmp _seh_restore_raw

// void RtlUnwindEx(frame, ip, rec, retval, CONTEXT* ctx, UNWIND_HISTORY_TABLE* table)
    .globl _RtlUnwindEx
    .p2align 4
_RtlUnwindEx:
    STUB_CAPTURE
    movq %rcx, %rdi
    movq %rdx, %rsi
    movq %r8, %rdx
    movq %r9, %rcx
    leaq STUB_FRAME(%rsp), %r8
    leaq STUB_CTX(%rsp), %r9
    call _seh_unwind_ex_entry
    ud2

// void RtlUnwind(frame, ip, rec, retval)
    .globl _RtlUnwind
    .p2align 4
_RtlUnwind:
    STUB_CAPTURE
    movq %rcx, %rdi
    movq %rdx, %rsi
    movq %r8, %rdx
    movq %r9, %rcx
    leaq STUB_CTX(%rsp), %r8
    call _seh_unwind_entry
    ud2

// Step-1 fallback for vcruntime140!_CxxThrowException(void* object, _ThrowInfo* info)
    .globl _gta6rt_seh_CxxThrowException
    .p2align 4
_gta6rt_seh_CxxThrowException:
    STUB_CAPTURE
    movq %rcx, %rdi
    movq %rdx, %rsi
    leaq STUB_CTX(%rsp), %rdx
    call _seh_cxx_throw_entry
    ud2

// WORD RtlCaptureStackBackTrace(DWORD skip, DWORD count, PVOID* trace, PDWORD hash)
    .globl _RtlCaptureStackBackTrace
    .p2align 4
_RtlCaptureStackBackTrace:
    STUB_CAPTURE
    movl %ecx, %edi
    movl %edx, %esi
    movq %r8, %rdx
    movq %r9, %rcx
    leaq STUB_CTX(%rsp), %r8
    call _seh_capture_backtrace_entry
    movq STUB_CTX+CTX_RSI(%rsp), %rsi
    movq STUB_CTX+CTX_RDI(%rsp), %rdi
    .irp r, 6,7,8,9,10,11,12,13,14,15
    movdqa STUB_CTX+CTX_XMM+\r*16(%rsp), %xmm\r
    .endr
    addq $(STUB_FRAME), %rsp
    ret

// Redirected hardware fault: entered by a jump (rsp 16-aligned) with rdi = seh fault frame.
    .globl _seh_fault_entry
    .p2align 4
_seh_fault_entry:
    call _seh_fault_dispatch_c
    ud2
