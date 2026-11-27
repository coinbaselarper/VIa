// void seh_test_enter_guest(uint64_t func, uint64_t rsp, const uint64_t regs[8], const uint8_t xmm[10][16])
// regs: rbx rbp rsi rdi r12 r13 r14 r15. Never returns; the test leaves through siglongjmp.
    .text
    .globl _seh_test_enter_guest
    .p2align 4
_seh_test_enter_guest:
    mov %rdi, %rax
    movdqu 0(%rcx), %xmm6
    movdqu 16(%rcx), %xmm7
    movdqu 32(%rcx), %xmm8
    movdqu 48(%rcx), %xmm9
    movdqu 64(%rcx), %xmm10
    movdqu 80(%rcx), %xmm11
    movdqu 96(%rcx), %xmm12
    movdqu 112(%rcx), %xmm13
    movdqu 128(%rcx), %xmm14
    movdqu 144(%rcx), %xmm15
    mov %rsi, %r11
    mov 0(%rdx), %rbx
    mov 8(%rdx), %rbp
    mov 16(%rdx), %rsi
    mov 24(%rdx), %rdi
    mov 32(%rdx), %r12
    mov 40(%rdx), %r13
    mov 48(%rdx), %r14
    mov 56(%rdx), %r15
    mov %r11, %rsp
    lea -0x200(%rsp), %rcx
    lea -0x300(%rsp), %rdx
    lea -0x400(%rsp), %r8
    lea -0x500(%rsp), %r9
    jmp *%rax
