// Test-only x86-64 code: a ymm harness (SysV) and hand-written Win64 "guest" functions (ms_abi).

.intel_syntax noprefix
.text

// void isa_test_run_ymm(const uint64_t in[16][4], uint64_t out[16][4], void* target)
// Loads ymm0-15, calls target (which must preserve rbx and rbp), stores ymm0-15.
.globl _isa_test_run_ymm
.p2align 4
_isa_test_run_ymm:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    mov rbx, rsi
    mov r12, rdx
    vmovdqu ymm0, [rdi + 0x000]
    vmovdqu ymm1, [rdi + 0x020]
    vmovdqu ymm2, [rdi + 0x040]
    vmovdqu ymm3, [rdi + 0x060]
    vmovdqu ymm4, [rdi + 0x080]
    vmovdqu ymm5, [rdi + 0x0a0]
    vmovdqu ymm6, [rdi + 0x0c0]
    vmovdqu ymm7, [rdi + 0x0e0]
    vmovdqu ymm8, [rdi + 0x100]
    vmovdqu ymm9, [rdi + 0x120]
    vmovdqu ymm10, [rdi + 0x140]
    vmovdqu ymm11, [rdi + 0x160]
    vmovdqu ymm12, [rdi + 0x180]
    vmovdqu ymm13, [rdi + 0x1a0]
    vmovdqu ymm14, [rdi + 0x1c0]
    vmovdqu ymm15, [rdi + 0x1e0]
    call r12
    vmovdqu [rbx + 0x000], ymm0
    vmovdqu [rbx + 0x020], ymm1
    vmovdqu [rbx + 0x040], ymm2
    vmovdqu [rbx + 0x060], ymm3
    vmovdqu [rbx + 0x080], ymm4
    vmovdqu [rbx + 0x0a0], ymm5
    vmovdqu [rbx + 0x0c0], ymm6
    vmovdqu [rbx + 0x0e0], ymm7
    vmovdqu [rbx + 0x100], ymm8
    vmovdqu [rbx + 0x120], ymm9
    vmovdqu [rbx + 0x140], ymm10
    vmovdqu [rbx + 0x160], ymm11
    vmovdqu [rbx + 0x180], ymm12
    vmovdqu [rbx + 0x1a0], ymm13
    vmovdqu [rbx + 0x1c0], ymm14
    vmovdqu [rbx + 0x1e0], ymm15
    vzeroupper
    pop r12
    pop rbx
    pop rbp
    ret

// ---- Win64 guest functions -------------------------------------------------------------------------

// uint64_t guest_sum16(a1..a16): checks entry alignment, scribbles over its home slots, returns the sum
// (or 0xA119 if rsp+8 was not 16-byte aligned).
.globl _guest_sum16
.p2align 4
_guest_sum16:
    lea rax, [rsp + 8]
    test rax, 15
    jnz 8f
    mov [rsp + 0x08], rcx
    mov [rsp + 0x10], rdx
    mov [rsp + 0x18], r8
    mov [rsp + 0x20], r9
    mov rax, [rsp + 0x08]
    add rax, [rsp + 0x10]
    add rax, [rsp + 0x18]
    add rax, [rsp + 0x20]
    mov r10, 0
1:
    add rax, [rsp + 0x28 + r10*8]
    inc r10
    cmp r10, 12
    jne 1b
    mov qword ptr [rsp + 0x08], -1
    mov qword ptr [rsp + 0x10], -1
    mov qword ptr [rsp + 0x18], -1
    mov qword ptr [rsp + 0x20], -1
    ret
8:
    mov eax, 0xA119
    ret

// double guest_fmix(int64 a, double b, int64 c, double d, double e /* stack */) = b*d + e + a + c
.globl _guest_fmix
.p2align 4
_guest_fmix:
    mulsd xmm1, xmm3
    addsd xmm1, qword ptr [rsp + 0x28]
    cvtsi2sd xmm2, rcx
    addsd xmm1, xmm2
    cvtsi2sd xmm2, r8
    addsd xmm1, xmm2
    movapd xmm0, xmm1
    ret

// void* guest_tls_entry(uint64_t index) = ((void**)gs:[0x58])[index]
.globl _guest_tls_entry
_guest_tls_entry:
    mov rax, qword ptr gs:[0x58]
    mov rax, [rax + rcx*8]
    ret

// uint64_t guest_stack_base(void) = ((NT_TIB*)gs:[0x30])->StackBase  (the title's only gs:[0x30] idiom)
.globl _guest_stack_base
_guest_stack_base:
    mov rax, qword ptr gs:[0x30]
    mov rax, [rax + 8]
    ret

.globl _guest_gs60
_guest_gs60:
    mov rax, qword ptr gs:[0x60]
    ret

.globl _guest_gs10
_guest_gs10:
    mov rax, qword ptr gs:[0x10]
    ret

// Double-prefix form seen 15 times in the title.
.globl _guest_gs58_dbl
_guest_gs58_dbl:
    .byte 0x65, 0x65, 0x48, 0x8b, 0x04, 0x25, 0x58, 0, 0, 0
    ret

// uint64_t guest_call6(fn, a1, a2, a3, a4, a5, a6): calls fn(a1..a6) the Win64 way (a1-a4 also in xmm0-3) with
// sentinels in every Win64 callee-saved register; returns fn's rax, or 0xBAD0+n if register n was clobbered.
.globl _guest_call6
.p2align 4
_guest_call6:
    push rbx
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15
    sub rsp, 0xD0
    movdqu [rsp + 0x30], xmm6
    movdqu [rsp + 0x40], xmm7
    movdqu [rsp + 0x50], xmm8
    movdqu [rsp + 0x60], xmm9
    movdqu [rsp + 0x70], xmm10
    movdqu [rsp + 0x80], xmm11
    movdqu [rsp + 0x90], xmm12
    movdqu [rsp + 0xa0], xmm13
    movdqu [rsp + 0xb0], xmm14
    movdqu [rsp + 0xc0], xmm15
    mov r11, rcx
    mov rax, [rsp + 0xD0 + 56 + 0x30]
    mov [rsp + 0x20], rax
    mov rax, [rsp + 0xD0 + 56 + 0x38]
    mov [rsp + 0x28], rax
    mov rcx, rdx
    mov rdx, r8
    mov r8, r9
    mov r9, [rsp + 0xD0 + 56 + 0x28]
    movq xmm0, rcx
    movq xmm1, rdx
    movq xmm2, r8
    movq xmm3, r9
    mov rbx, 0x1111111111111101
    mov rsi, 0x1111111111111102
    mov rdi, 0x1111111111111103
    mov r12, 0x1111111111111104
    mov r13, 0x1111111111111105
    mov r14, 0x1111111111111106
    mov r15, 0x1111111111111107
    mov r10, 0x6060606060606006
    movq xmm6, r10
    pinsrq xmm6, r10, 1
    inc r10
    movq xmm7, r10
    pinsrq xmm7, r10, 1
    inc r10
    movq xmm8, r10
    pinsrq xmm8, r10, 1
    inc r10
    movq xmm9, r10
    pinsrq xmm9, r10, 1
    inc r10
    movq xmm10, r10
    pinsrq xmm10, r10, 1
    inc r10
    movq xmm11, r10
    pinsrq xmm11, r10, 1
    inc r10
    movq xmm12, r10
    pinsrq xmm12, r10, 1
    inc r10
    movq xmm13, r10
    pinsrq xmm13, r10, 1
    inc r10
    movq xmm14, r10
    pinsrq xmm14, r10, 1
    inc r10
    movq xmm15, r10
    pinsrq xmm15, r10, 1
    call r11
    mov r10, 0x1111111111111101
    cmp rbx, r10
    jne 7f
    inc r10
    cmp rsi, r10
    jne 7f
    inc r10
    cmp rdi, r10
    jne 7f
    inc r10
    cmp r12, r10
    jne 7f
    inc r10
    cmp r13, r10
    jne 7f
    inc r10
    cmp r14, r10
    jne 7f
    inc r10
    cmp r15, r10
    jne 7f
    mov r10, 0x6060606060606006
.macro CHKX reg
    movq r11, \reg
    cmp r11, r10
    jne 6f
    pextrq r11, \reg, 1
    cmp r11, r10
    jne 6f
    inc r10
.endm
    CHKX xmm6
    CHKX xmm7
    CHKX xmm8
    CHKX xmm9
    CHKX xmm10
    CHKX xmm11
    CHKX xmm12
    CHKX xmm13
    CHKX xmm14
    CHKX xmm15
    jmp 5f
6:
    mov eax, 0xBADC
    jmp 5f
7:
    mov rax, r10
    and eax, 0xf
    add eax, 0xBAD0
5:
    movdqu xmm6, [rsp + 0x30]
    movdqu xmm7, [rsp + 0x40]
    movdqu xmm8, [rsp + 0x50]
    movdqu xmm9, [rsp + 0x60]
    movdqu xmm10, [rsp + 0x70]
    movdqu xmm11, [rsp + 0x80]
    movdqu xmm12, [rsp + 0x90]
    movdqu xmm13, [rsp + 0xa0]
    movdqu xmm14, [rsp + 0xb0]
    movdqu xmm15, [rsp + 0xc0]
    add rsp, 0xD0
    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbx
    ret

// uint64_t guest_fault_null(void): reads [0x10] (4-byte insn 48 8b 04 25 ... is 8 bytes); a handler that
// skips 8 bytes and sets rax makes this return rax.
.globl _guest_fault_null
_guest_fault_null:
    xor eax, eax
    .byte 0x48, 0x8b, 0x04, 0x25, 0x10, 0, 0, 0
    ret

.globl _guest_int3
_guest_int3:
    xor eax, eax
    int3
    ret

.globl _guest_ud2
_guest_ud2:
    xor eax, eax
    ud2
    ret

// void isa_test_run_gpr(uint64_t r[4] /* rax rbx rcx rdx, in/out */, void* target)
.globl _isa_test_run_gpr
.p2align 4
_isa_test_run_gpr:
    push rbx
    push r12
    push r13
    mov r12, rdi
    mov r13, rsi
    mov rax, [r12]
    mov rbx, [r12 + 8]
    mov rcx, [r12 + 16]
    mov rdx, [r12 + 24]
    call r13
    mov [r12], rax
    mov [r12 + 8], rbx
    mov [r12 + 16], rcx
    mov [r12 + 24], rdx
    pop r13
    pop r12
    pop rbx
    ret

// uint64_t isa_test_chkstk(void* chkstk, uint64_t size): calls __chkstk with rax = size and returns 0 if rsp,
// r10, r11 and rax came back unchanged, else a bit mask of what changed.
.globl _isa_test_chkstk
.p2align 4
_isa_test_chkstk:
    push rbx
    push r12
    mov rbx, rsp
    mov r10, 0x1111111111111111
    mov r11, 0x2222222222222222
    mov r12, rsi
    mov rax, rsi
    call rdi
    xor ecx, ecx
    cmp rsp, rbx
    je 1f
    or ecx, 1
1:
    mov rdx, 0x1111111111111111
    cmp r10, rdx
    je 2f
    or ecx, 2
2:
    mov rdx, 0x2222222222222222
    cmp r11, rdx
    je 3f
    or ecx, 4
3:
    cmp rax, r12
    je 4f
    or ecx, 8
4:
    mov eax, ecx
    pop r12
    pop rbx
    ret
