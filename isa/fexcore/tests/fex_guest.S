// Guest x86-64 test code (Win64 ABI). Assembled for x86_64 and copied into data memory by fex_blob.py: the arm64
// test only ever runs it through FEX. The same object is linked natively into the x86_64 Rosetta benchmark.
        .intel_syntax noprefix
        .text

// uint64_t g_sum16(a1..a16): sum of (i+1)*ai; bit 63 set when rsp+8 was not 16-byte aligned at entry.
        .p2align 4
        .globl _g_sum16
_g_sum16:
        mov     qword ptr [rsp + 8], -1
        mov     qword ptr [rsp + 16], -1
        mov     qword ptr [rsp + 24], -1
        mov     qword ptr [rsp + 32], -1
        mov     rax, rcx
        lea     rax, [rax + rdx*2]
        imul    r8, r8, 3
        add     rax, r8
        shl     r9, 2
        add     rax, r9
        mov     r10, 5
        lea     r11, [rsp + 0x28]
1:
        mov     rcx, qword ptr [r11]
        imul    rcx, r10
        add     rax, rcx
        add     r11, 8
        inc     r10
        cmp     r10, 17
        jne     1b
        lea     rcx, [rsp + 8]
        test    rcx, 15
        jz      2f
        bts     rax, 63
2:
        ret

// double g_fmix(double a, int64 b, double c, int64 d, double e): a*2 + b + c*3 + d + e
        .p2align 4
        .globl _g_fmix
_g_fmix:
        addsd   xmm0, xmm0
        cvtsi2sd xmm1, rdx
        addsd   xmm0, xmm1
        mulsd   xmm2, qword ptr [rip + L_three]
        addsd   xmm0, xmm2
        cvtsi2sd xmm1, r9
        addsd   xmm0, xmm1
        addsd   xmm0, qword ptr [rsp + 0x28]
        ret

        .p2align 3
L_three:
        .quad   0x4008000000000000

// uint64_t g_call6(thunk, a, b, c, d, e): calls thunk(a, b, c, d, e, 0x66) with callee-saved registers set to
// known values; returns the thunk's rax, or 0xBAD0000 | register number if one was not preserved.
        .p2align 4
        .globl _g_call6
_g_call6:
        push    rbx
        push    rbp
        push    rsi
        push    rdi
        push    r12
        push    r13
        push    r14
        push    r15
        sub     rsp, 0xA8
        movdqu  [rsp + 0x30], xmm6
        movdqu  [rsp + 0x40], xmm7
        movdqu  [rsp + 0x50], xmm8
        movdqu  [rsp + 0x60], xmm15
        mov     rax, rcx
        mov     rbx, 0x1111111111111111
        mov     rbp, 0x2222222222222222
        mov     rsi, 0x3333333333333333
        mov     rdi, 0x4444444444444444
        mov     r12, 0x5555555555555555
        mov     r13, 0x6666666666666666
        mov     r14, 0x7777777777777777
        mov     r15, 0x0123456789abcdef
        movq    xmm6, rbx
        movq    xmm7, rbp
        movq    xmm8, rsi
        movq    xmm15, r15
        mov     rcx, rdx
        mov     rdx, r8
        mov     r8, r9
        mov     r9, qword ptr [rsp + 0xA8 + 0x40 + 0x28]
        mov     r10, qword ptr [rsp + 0xA8 + 0x40 + 0x30]
        mov     qword ptr [rsp + 0x20], r10
        mov     qword ptr [rsp + 0x28], 0x66
        call    rax
        mov     rcx, 0xBAD0003
        mov     rdx, 0x1111111111111111
        cmp     rbx, rdx
        jne     9f
        mov     rcx, 0xBAD0005
        mov     rdx, 0x2222222222222222
        cmp     rbp, rdx
        jne     9f
        mov     rcx, 0xBAD0006
        mov     rdx, 0x3333333333333333
        cmp     rsi, rdx
        jne     9f
        mov     rcx, 0xBAD0007
        mov     rdx, 0x4444444444444444
        cmp     rdi, rdx
        jne     9f
        mov     rcx, 0xBAD000C
        mov     rdx, 0x5555555555555555
        cmp     r12, rdx
        jne     9f
        mov     rcx, 0xBAD000D
        mov     rdx, 0x6666666666666666
        cmp     r13, rdx
        jne     9f
        mov     rcx, 0xBAD000E
        mov     rdx, 0x7777777777777777
        cmp     r14, rdx
        jne     9f
        mov     rcx, 0xBAD000F
        mov     rdx, 0x0123456789abcdef
        cmp     r15, rdx
        jne     9f
        mov     rcx, 0xBAD0016
        movq    rdx, xmm6
        cmp     rdx, rbx
        jne     9f
        mov     rcx, 0xBAD0017
        movq    rdx, xmm7
        cmp     rdx, rbp
        jne     9f
        mov     rcx, 0xBAD0018
        movq    rdx, xmm8
        cmp     rdx, rsi
        jne     9f
        mov     rcx, 0xBAD001F
        movq    rdx, xmm15
        cmp     rdx, r15
        jne     9f
        jmp     8f
9:
        mov     rax, rcx
8:
        movdqu  xmm6, [rsp + 0x30]
        movdqu  xmm7, [rsp + 0x40]
        movdqu  xmm8, [rsp + 0x50]
        movdqu  xmm15, [rsp + 0x60]
        add     rsp, 0xA8
        pop     r15
        pop     r14
        pop     r13
        pop     r12
        pop     rdi
        pop     rsi
        pop     rbp
        pop     rbx
        ret

// double g_call_float(thunk): returns thunk(1.5, 2, 3.25, 4) + 0.5 (args 0 and 2 in xmm, 1 and 3 in GPRs)
        .p2align 4
        .globl _g_call_float
_g_call_float:
        sub     rsp, 0x28
        mov     rax, rcx
        movsd   xmm0, qword ptr [rip + L_1p5]
        mov     rdx, 2
        movsd   xmm2, qword ptr [rip + L_3p25]
        mov     r9, 4
        call    rax
        addsd   xmm0, qword ptr [rip + L_0p5]
        add     rsp, 0x28
        ret
        .p2align 3
L_1p5:  .quad   0x3FF8000000000000
L_3p25: .quad   0x400A000000000000
L_0p5:  .quad   0x3FE0000000000000

// uint64_t g_add3(a, b, c): a + 3*b + c, with a stack frame and a call to a leaf.
        .p2align 4
        .globl _g_add3
_g_add3:
        sub     rsp, 0x28
        lea     rax, [rdx + rdx*2]
        add     rax, rcx
        add     rax, r8
        add     rsp, 0x28
        ret

// uint64_t g_call1(thunk, a): thunk(a) + 1000
        .p2align 4
        .globl _g_call1
_g_call1:
        sub     rsp, 0x28
        mov     rax, rcx
        mov     rcx, rdx
        call    rax
        add     rax, 1000
        add     rsp, 0x28
        ret

// void g_gs_probe(uint64_t out[6]): gs:[0x30], gs:[0x08], gs:[0x10], gs:[0x60], gs:[0x58]->[0], gs:[0x00]
        .p2align 4
        .globl _g_gs_probe
_g_gs_probe:
        mov     rax, qword ptr gs:[0x30]
        mov     qword ptr [rcx], rax
        mov     rax, qword ptr gs:[0x08]
        mov     qword ptr [rcx + 8], rax
        mov     rax, qword ptr gs:[0x10]
        mov     qword ptr [rcx + 16], rax
        mov     rax, qword ptr gs:[0x60]
        mov     qword ptr [rcx + 24], rax
        mov     rax, qword ptr gs:[0x58]
        mov     rax, qword ptr [rax]
        mov     qword ptr [rcx + 32], rax
        mov     rax, qword ptr gs:[0x00]
        mov     qword ptr [rcx + 40], rax
        mov     rax, rsp
        ret

// Faulting instructions; the test's handler resumes at the label after each one with rax = 0x600D.
        .p2align 4
        .globl _g_fault_ud2
_g_fault_ud2:
        xor     eax, eax
        .globl _g_fault_ud2_site
_g_fault_ud2_site:
        ud2
        ret

        .p2align 4
        .globl _g_fault_int3
_g_fault_int3:
        xor     eax, eax
        .globl _g_fault_int3_site
_g_fault_int3_site:
        int3
        ret

        .p2align 4
        .globl _g_fault_read
_g_fault_read:
        xor     eax, eax
        .globl _g_fault_read_site
_g_fault_read_site:
        mov     rax, qword ptr [rcx]
        ret

        .p2align 4
        .globl _g_fault_write
_g_fault_write:
        xor     eax, eax
        .globl _g_fault_write_site
_g_fault_write_site:
        mov     qword ptr [rcx], rdx
        ret

        .p2align 4
        .globl _g_div
_g_div:
        mov     rax, rcx
        xor     edx, edx
        div     r8
        ret

        .p2align 4
        .globl _g_fastfail
_g_fastfail:
        mov     ecx, 7
        .globl _g_fastfail_site
_g_fastfail_site:
        int     0x29
        ret

// AMD/Zen instructions the title executes. rcx = 64-byte aligned buffer.
        .p2align 4
        .globl _g_insertq
_g_insertq:
        movq    xmm0, rcx
        movq    xmm1, rdx
        insertq xmm0, xmm1, 16, 8
        movq    rax, xmm0
        ret

        .p2align 4
        .globl _g_extrq
_g_extrq:
        movq    xmm0, rcx
        extrq   xmm0, 12, 20
        movq    rax, xmm0
        ret

        .p2align 4
        .globl _g_clzero
_g_clzero:
        mov     rax, rcx
        clzero
        mov     rax, qword ptr [rcx + 56]
        ret

        .p2align 4
        .globl _g_clwb
_g_clwb:
        clwb    [rcx]
        clflushopt [rcx]
        mov     rax, qword ptr [rcx]
        ret

        .p2align 4
        .globl _g_monitorx
_g_monitorx:
        mov     rax, rcx
        xor     ecx, ecx
        xor     edx, edx
        .byte   0x0f, 0x01, 0xfa
        xor     eax, eax
        .byte   0x0f, 0x01, 0xfb
        mov     eax, 0x77
        ret

// vpsllvq/vpsrlvq with counts in [rdx]: data [rcx] (4 qwords), results to [r8], [r8+32]
        .p2align 4
        .globl _g_vshift
_g_vshift:
        vmovdqu ymm0, [rcx]
        vmovdqu ymm1, [rdx]
        vpsllvq ymm2, ymm0, ymm1
        vpsrlvq ymm3, ymm0, ymm1
        vmovdqu [r8], ymm2
        vmovdqu [r8 + 32], ymm3
        vzeroupper
        ret

// void g_cpuid(leaf, subleaf, out[4])
        .p2align 4
        .globl _g_cpuid
_g_cpuid:
        push    rbx
        mov     eax, ecx
        mov     ecx, edx
        cpuid
        mov     dword ptr [r8], eax
        mov     dword ptr [r8 + 4], ebx
        mov     dword ptr [r8 + 8], ecx
        mov     dword ptr [r8 + 12], edx
        pop     rbx
        ret

        .p2align 4
        .globl _g_xgetbv
_g_xgetbv:
        xor     ecx, ecx
        xgetbv
        shl     rdx, 32
        or      rax, rdx
        ret

// uint64_t g_call_chkstk(fn, size): calls an MSVC __chkstk with rax = size; 0 when rsp/r10/r11 survive
        .p2align 4
        .globl _g_call_chkstk
_g_call_chkstk:
        push    rbx
        push    r12
        push    r13
        sub     rsp, 0x20
        mov     rbx, rsp
        mov     r10, 0x1010
        mov     r11, 0x1111
        mov     rax, rdx
        call    rcx
        xor     eax, eax
        cmp     rsp, rbx
        setne   al
        cmp     r10, 0x1010
        setne   r12b
        or      al, r12b
        cmp     r11, 0x1111
        setne   r13b
        or      al, r13b
        add     rsp, 0x20
        pop     r13
        pop     r12
        pop     rbx
        ret

// uint64_t g_lock_add(uint64_t* p, n, delta): n times lock xadd [p], delta (p may be misaligned)
        .p2align 4
        .globl _g_lock_add
_g_lock_add:
        mov     rax, r8
1:
        test    rdx, rdx
        jz      2f
        lock xadd qword ptr [rcx], rax
        mov     rax, r8
        dec     rdx
        jmp     1b
2:
        mov     rax, qword ptr [rcx]
        ret


// void g_call_win64(const uint64_t* in, uint64_t* out): a generic Win64 call. in[0] target, in[1..4] rcx rdx r8 r9,
// in[5..8] xmm0..xmm3 (low 64 bits), in[9] number of stack arguments, in[10..] stack arguments.
// out[0] = rax, out[1..2] = xmm0 (128 bits).
        .p2align 4
        .globl _g_call_win64
_g_call_win64:
        push    rbx
        push    rsi
        push    rdi
        push    rbp
        mov     rbp, rsp
        mov     rbx, rcx
        mov     rsi, rdx
        mov     rcx, qword ptr [rbx + 72]
        lea     rax, [rcx*8 + 0x30]
        sub     rsp, rax
        and     rsp, -16
        xor     edx, edx
1:
        cmp     rdx, rcx
        jae     2f
        mov     rax, qword ptr [rbx + 80 + rdx*8]
        mov     qword ptr [rsp + 0x20 + rdx*8], rax
        inc     rdx
        jmp     1b
2:
        movq    xmm0, qword ptr [rbx + 40]
        movq    xmm1, qword ptr [rbx + 48]
        movq    xmm2, qword ptr [rbx + 56]
        movq    xmm3, qword ptr [rbx + 64]
        mov     rcx, qword ptr [rbx + 8]
        mov     rdx, qword ptr [rbx + 16]
        mov     r8, qword ptr [rbx + 24]
        mov     r9, qword ptr [rbx + 32]
        call    qword ptr [rbx]
        mov     qword ptr [rsi], rax
        movdqu  xmmword ptr [rsi + 8], xmm0
        mov     rsp, rbp
        pop     rbp
        pop     rdi
        pop     rsi
        pop     rbx
        ret

// ---- benchmark kernels -------------------------------------------------------------------------------------

// uint64_t g_bench_int(n, seed): LCG chain with mixing
        .p2align 4
        .globl _g_bench_int
_g_bench_int:
        mov     rax, rdx
        mov     r8, 6364136223846793005
        mov     r9, 1442695040888963407
1:
        test    rcx, rcx
        jz      2f
        imul    rax, r8
        add     rax, r9
        mov     r10, rax
        shr     r10, 29
        xor     rax, r10
        dec     rcx
        jmp     1b
2:
        ret

// uint64_t g_bench_mem(uint64_t* a, reps, len): reps passes of a[i] = a[i]*3 + a[i-1], returns sum
        .p2align 4
        .globl _g_bench_mem
_g_bench_mem:
        push    rbx
        xor     eax, eax
1:
        test    rdx, rdx
        jz      4f
        mov     r10, 1
        mov     r11, qword ptr [rcx]
2:
        cmp     r10, r8
        jae     3f
        mov     rbx, qword ptr [rcx + r10*8]
        lea     rbx, [rbx + rbx*2]
        add     rbx, r11
        mov     qword ptr [rcx + r10*8], rbx
        mov     r11, rbx
        inc     r10
        jmp     2b
3:
        add     rax, r11
        dec     rdx
        jmp     1b
4:
        pop     rbx
        ret

// double g_bench_sse(const float* a, const float* b, reps, len): dot products with mulps/addps
        .p2align 4
        .globl _g_bench_sse
_g_bench_sse:
        xorps   xmm0, xmm0
        xorps   xmm3, xmm3
1:
        test    r8, r8
        jz      3f
        xor     eax, eax
2:
        movups  xmm1, [rcx + rax*4]
        movups  xmm2, [rdx + rax*4]
        mulps   xmm1, xmm2
        addps   xmm0, xmm1
        movups  xmm1, [rcx + rax*4 + 16]
        movups  xmm2, [rdx + rax*4 + 16]
        mulps   xmm1, xmm2
        addps   xmm3, xmm1
        add     rax, 8
        cmp     rax, r9
        jb      2b
        dec     r8
        jmp     1b
3:
        addps   xmm0, xmm3
        movaps  xmm1, xmm0
        shufps  xmm1, xmm0, 0x4e
        addps   xmm0, xmm1
        movaps  xmm1, xmm0
        shufps  xmm1, xmm0, 0xb1
        addss   xmm0, xmm1
        cvtss2sd xmm0, xmm0
        ret

// uint32_t g_bench_avx2(const int32_t* a, const int32_t* b, reps, len): vpmulld/vpaddd/vpxor reduction
        .p2align 4
        .globl _g_bench_avx2
_g_bench_avx2:
        vpxor   ymm0, ymm0, ymm0
1:
        test    r8, r8
        jz      3f
        xor     eax, eax
2:
        vmovdqu ymm1, [rcx + rax*4]
        vpmulld ymm1, ymm1, [rdx + rax*4]
        vpaddd  ymm0, ymm0, ymm1
        vpsrld  ymm2, ymm0, 7
        vpxor   ymm0, ymm0, ymm2
        add     rax, 8
        cmp     rax, r9
        jb      2b
        dec     r8
        jmp     1b
3:
        vextracti128 xmm1, ymm0, 1
        vpaddd  xmm0, xmm0, xmm1
        vpshufd xmm1, xmm0, 0x4e
        vpaddd  xmm0, xmm0, xmm1
        vpshufd xmm1, xmm0, 0xb1
        vpaddd  xmm0, xmm0, xmm1
        vmovd   eax, xmm0
        vzeroupper
        ret

// uint64_t g_bench_calls(thunk, n): n calls of thunk(i), summing results
        .p2align 4
        .globl _g_bench_calls
_g_bench_calls:
        push    rbx
        push    rsi
        push    rdi
        sub     rsp, 0x20
        mov     rbx, rcx
        mov     rsi, rdx
        xor     edi, edi
1:
        test    rsi, rsi
        jz      2f
        mov     rcx, rsi
        call    rbx
        add     rdi, rax
        dec     rsi
        jmp     1b
2:
        mov     rax, rdi
        add     rsp, 0x20
        pop     rdi
        pop     rsi
        pop     rbx
        ret

// uint64_t g_bench_mix(n): branchy integer code with calls (a stand-in for game logic)
        .p2align 4
        .globl _g_bench_mix
_g_bench_mix:
        push    rbx
        xor     eax, eax
        mov     rbx, rcx
1:
        test    rbx, rbx
        jz      3f
        mov     rcx, rbx
        call    L_mix_leaf
        add     rax, rdx
        dec     rbx
        jmp     1b
3:
        pop     rbx
        ret
L_mix_leaf:
        mov     rdx, rcx
        and     rdx, 7
        cmp     rdx, 3
        jb      4f
        imul    rdx, rcx
        ror     rdx, 7
        ret
4:
        lea     rdx, [rcx + rcx*4]
        xor     rdx, 0x5a5a
        ret

// uint32_t g_set_mxcsr(uint32_t v): ldmxcsr v, returns the previous MXCSR
        .p2align 4
        .globl _g_set_mxcsr
_g_set_mxcsr:
        stmxcsr dword ptr [rsp + 16]
        mov     dword ptr [rsp + 8], ecx
        ldmxcsr dword ptr [rsp + 8]
        mov     eax, dword ptr [rsp + 16]
        ret

// uint64_t g_divsd(uint64_t a_bits, uint64_t b_bits): bits of a / b under the current MXCSR rounding
        .p2align 4
        .globl _g_divsd
_g_divsd:
        movq    xmm0, rcx
        movq    xmm1, rdx
        divsd   xmm0, xmm1
        movq    rax, xmm0
        ret

// uint64_t g_mx_nested(thunk, uint32_t mxcsr, a_bits, b_bits, uint64_t* out): sets MXCSR, calls thunk(a_bits, b_bits),
// stores the bits of a / b computed after the thunk returns to *out, restores MXCSR; returns the thunk's rax
        .p2align 4
        .globl _g_mx_nested
_g_mx_nested:
        push    rbx
        push    rsi
        push    rdi
        sub     rsp, 0x40
        mov     rbx, r8
        mov     rsi, r9
        mov     rdi, qword ptr [rsp + 0x40 + 0x18 + 0x28]
        stmxcsr dword ptr [rsp + 0x30]
        mov     dword ptr [rsp + 0x28], edx
        ldmxcsr dword ptr [rsp + 0x28]
        mov     rax, rcx
        mov     rcx, rbx
        mov     rdx, rsi
        call    rax
        movq    xmm0, rbx
        movq    xmm1, rsi
        divsd   xmm0, xmm1
        movq    qword ptr [rdi], xmm0
        ldmxcsr dword ptr [rsp + 0x30]
        add     rsp, 0x40
        pop     rdi
        pop     rsi
        pop     rbx
        ret

// uint64_t g_ctx_probe(thunk, uint64_t out[2]): rbx, rbp, rsi, rdi, r12-r15 = 0x11..0x88 patterns, then calls
// thunk; out[0] = rsp at the call, out[1] = its return address. Returns the thunk's rax.
        .p2align 4
        .globl _g_ctx_probe
_g_ctx_probe:
        push    rbx
        push    rbp
        push    rsi
        push    rdi
        push    r12
        push    r13
        push    r14
        push    r15
        sub     rsp, 0x28
        mov     rax, rcx
        mov     qword ptr [rdx], rsp
        lea     r10, [rip + 1f]
        mov     qword ptr [rdx + 8], r10
        movabs  rbx, 0x1111111111111111
        movabs  rbp, 0x2222222222222222
        movabs  rsi, 0x3333333333333333
        movabs  rdi, 0x4444444444444444
        movabs  r12, 0x5555555555555555
        movabs  r13, 0x6666666666666666
        movabs  r14, 0x7777777777777777
        movabs  r15, 0x8888888888888888
        call    rax
1:
        add     rsp, 0x28
        pop     r15
        pop     r14
        pop     r13
        pop     r12
        pop     rdi
        pop     rsi
        pop     rbp
        pop     rbx
        ret

// uint64_t g_resume_probe(thunk): rbx = 0x5151, calls thunk; a thunk that resumes at g_resume_landing with rax = X
// makes this return X + 0x5151; a thunk that returns normally makes it return 0xBAD.
        .p2align 4
        .globl _g_resume_probe
_g_resume_probe:
        push    rbx
        sub     rsp, 0x20
        mov     ebx, 0x5151
        call    rcx
        mov     eax, 0xBAD
        jmp     2f
        .globl _g_resume_landing
_g_resume_landing:
        add     rax, rbx
2:
        add     rsp, 0x20
        pop     rbx
        ret
