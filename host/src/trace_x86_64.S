// Import-call trace trampoline (x86_64 only). Entered from a per-import stub with r10 = trace record (target at +0)
// and the guest's stack untouched: counts the call through an ms_abi host function, restores the argument registers
// and tail-jumps to the real binding, so the callee sees the guest's own frame.

.intel_syntax noprefix
.text

.globl _gta6rt_host_trace_tramp
.p2align 4
_gta6rt_host_trace_tramp:
    sub rsp, 0x98
    mov qword ptr [rsp + 0x20], rcx
    mov qword ptr [rsp + 0x28], rdx
    mov qword ptr [rsp + 0x30], r8
    mov qword ptr [rsp + 0x38], r9
    movdqu xmmword ptr [rsp + 0x40], xmm0
    movdqu xmmword ptr [rsp + 0x50], xmm1
    movdqu xmmword ptr [rsp + 0x60], xmm2
    movdqu xmmword ptr [rsp + 0x70], xmm3
    mov qword ptr [rsp + 0x80], r10
    mov qword ptr [rsp + 0x88], rax
    mov rcx, r10
    lea rdx, [rsp + 0x20]
    mov r8, qword ptr [rsp + 0x98]
    call _gta6rt_host_trace_hit
    mov r10, qword ptr [rsp + 0x80]
    mov rax, qword ptr [rsp + 0x88]
    mov rcx, qword ptr [rsp + 0x20]
    mov rdx, qword ptr [rsp + 0x28]
    mov r8, qword ptr [rsp + 0x30]
    mov r9, qword ptr [rsp + 0x38]
    movdqu xmm0, xmmword ptr [rsp + 0x40]
    movdqu xmm1, xmmword ptr [rsp + 0x50]
    movdqu xmm2, xmmword ptr [rsp + 0x60]
    movdqu xmm3, xmmword ptr [rsp + 0x70]
    add rsp, 0x98
    jmp qword ptr [r10]
