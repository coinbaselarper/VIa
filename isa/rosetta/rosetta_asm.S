// Guest <-> host bridges for the x86_64 (Rosetta) backend. Offsets into the TEB private area must match
// common/isa_internal.h: TebPrivate at TEB+0x1900 (mode +0x08, host_tsd +0x10, depth +0x28, scratch +0x30,
// frames +0x70, 32 bytes each).

.intel_syntax noprefix
.text

#define TP_MODE 0x1908
#define TP_HOST_TSD 0x1910
#define TP_DEPTH 0x1928
#define TP_SCRATCH 0x1930
#define TP_FRAMES 0x1970
#define TEB_MODE_SWAP 1
#define SYS_THREAD_SET_TSD_BASE 0x3000003

// SysV: rdi = target, rsi = uint64_t args[16], edx = count, rcx = gta6rt_guest_ret*.
// Win64 call with 32 bytes of home space and stack args at [rsp+0x20+8*(i-4)]; rsp is 16-byte aligned at the
// call, so the callee sees the usual rsp % 16 == 8. Args 1-4 go in both the GPR and the xmm register.
.globl _gta6rt_rosetta_call_guest_asm
.p2align 4
_gta6rt_rosetta_call_guest_asm:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    mov r12, rdi
    mov r13, rsi
    mov r14, rcx
    mov ebx, edx
    mov eax, ebx
    cmp eax, 4
    jae 1f
    mov eax, 4
1:
    lea rax, [rax*8 + 15]
    and rax, -16
    sub rsp, rax
    and rsp, -16
    mov ecx, 4
2:
    cmp ecx, ebx
    jae 3f
    mov rax, qword ptr [r13 + rcx*8]
    mov qword ptr [rsp + rcx*8], rax
    inc ecx
    jmp 2b
3:
    mov rcx, qword ptr [r13]
    mov rdx, qword ptr [r13 + 8]
    mov r8, qword ptr [r13 + 16]
    mov r9, qword ptr [r13 + 24]
    movq xmm0, rcx
    movq xmm1, rdx
    movq xmm2, r8
    movq xmm3, r9
    call r12
    mov qword ptr [r14], rax
    movq qword ptr [r14 + 8], xmm0
    lea rsp, [rbp - 32]
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

// NATIVE thunk target. r10 = thunk record (fn at +0). Under SHARE (or with no TEB) this is a tail jump, so the
// host ms_abi function sees exactly the guest's frame. Under SWAP the return address and the registers the
// syscall clobbers are parked in the TEB so the host function still sees the guest frame unchanged.
.globl _gta6rt_rosetta_native_entry
.p2align 4
_gta6rt_rosetta_native_entry:
    mov r11, qword ptr gs:[0x30]
    test r11, r11
    jz 1f
    cmp dword ptr [r11 + TP_MODE], TEB_MODE_SWAP
    je 2f
1:
    jmp qword ptr [r10]
2:
    mov rax, qword ptr [r11 + TP_DEPTH]
    cmp rax, 64
    jae 9f
    shl rax, 5
    lea rax, [r11 + rax + TP_FRAMES]
    pop qword ptr [rax]
    mov qword ptr [rax + 8], rbx
    mov qword ptr [rax + 16], rsi
    mov qword ptr [rax + 24], rdi
    inc qword ptr [r11 + TP_DEPTH]
    mov rbx, r11
    mov qword ptr [rbx + TP_SCRATCH], rcx
    mov qword ptr [rbx + TP_SCRATCH + 8], rdx
    mov qword ptr [rbx + TP_SCRATCH + 16], r8
    mov qword ptr [rbx + TP_SCRATCH + 24], r9
    mov qword ptr [rbx + TP_SCRATCH + 32], r10
    mov rdi, qword ptr [rbx + TP_HOST_TSD]
    xor esi, esi
    mov eax, SYS_THREAD_SET_TSD_BASE
    syscall
    mov rcx, qword ptr [rbx + TP_SCRATCH]
    mov rdx, qword ptr [rbx + TP_SCRATCH + 8]
    mov r8, qword ptr [rbx + TP_SCRATCH + 16]
    mov r9, qword ptr [rbx + TP_SCRATCH + 24]
    mov r10, qword ptr [rbx + TP_SCRATCH + 32]
    call qword ptr [r10]
    mov qword ptr [rbx + TP_SCRATCH], rax
    mov rdi, rbx
    xor esi, esi
    mov eax, SYS_THREAD_SET_TSD_BASE
    syscall
    mov rax, qword ptr [rbx + TP_SCRATCH]
    dec qword ptr [rbx + TP_DEPTH]
    mov rcx, qword ptr [rbx + TP_DEPTH]
    shl rcx, 5
    lea rcx, [rbx + rcx + TP_FRAMES]
    mov rsi, qword ptr [rcx + 16]
    mov rdi, qword ptr [rcx + 24]
    push qword ptr [rcx]
    mov rbx, qword ptr [rcx + 8]
    ret
9:
    ud2

// FRAME/TRAP thunk target. r10 = thunk record. Builds a gta6rt_guest_frame and calls the ms_abi dispatcher,
// which preserves the guest's callee-saved registers (rbx rbp rdi rsi r12-r15 xmm6-15) by its own ABI.
// Frame layout at rsp+0x20: rcx rdx r8 r9 | xmm0-3 lo | rsp | rax | xmm0_ret | ret_is_float,reserved
.globl _gta6rt_rosetta_frame_entry
.p2align 4
_gta6rt_rosetta_frame_entry:
    push rbp
    mov rbp, rsp
    and rsp, -16
    sub rsp, 0x80
    mov qword ptr [rsp + 0x20], rcx
    mov qword ptr [rsp + 0x28], rdx
    mov qword ptr [rsp + 0x30], r8
    mov qword ptr [rsp + 0x38], r9
    movq qword ptr [rsp + 0x40], xmm0
    movq qword ptr [rsp + 0x48], xmm1
    movq qword ptr [rsp + 0x50], xmm2
    movq qword ptr [rsp + 0x58], xmm3
    lea rax, [rbp + 8]
    mov qword ptr [rsp + 0x60], rax
    xor eax, eax
    mov qword ptr [rsp + 0x68], rax
    mov qword ptr [rsp + 0x70], rax
    mov qword ptr [rsp + 0x78], rax
    lea rcx, [rsp + 0x20]
    mov rdx, r10
    call _gta6rt_rosetta_frame_dispatch
    mov rax, qword ptr [rsp + 0x68]
    movq xmm0, qword ptr [rsp + 0x70]
    mov rsp, rbp
    pop rbp
    ret
