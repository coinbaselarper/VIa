#pragma once

/*
 * Guest x86-64 machine state exchanged between isa/ and seh/. gta6rt_x64_context has exactly the layout of
 * the Windows x64 CONTEXT (1232 bytes, 16-byte aligned), and gta6rt_x64_exception_record that of
 * EXCEPTION_RECORD (152 bytes), so seh/ can pass them to guest handlers unchanged. compat/tests asserts this.
 */

#include <gta6rt/abi.h>

typedef struct __attribute__((aligned(16))) gta6rt_m128 {
    uint64_t lo;
    int64_t hi;
} gta6rt_m128;

typedef struct __attribute__((aligned(16))) gta6rt_x64_context {
    uint64_t p_home[6];
    uint32_t context_flags;
    uint32_t mxcsr;
    uint16_t seg_cs, seg_ds, seg_es, seg_fs, seg_gs, seg_ss;
    uint32_t eflags;
    uint64_t dr0, dr1, dr2, dr3, dr6, dr7;
    uint64_t rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip;
    union {
        uint8_t flt_save[512];
        struct {
            gta6rt_m128 header[2];
            gta6rt_m128 legacy[8];
            gta6rt_m128 xmm[16];
        } v;
    } fp;
    gta6rt_m128 vector_register[26];
    uint64_t vector_control;
    uint64_t debug_control;
    uint64_t last_branch_to_rip;
    uint64_t last_branch_from_rip;
    uint64_t last_exception_to_rip;
    uint64_t last_exception_from_rip;
} gta6rt_x64_context;

#define GTA6RT_X64_CONTEXT_SIZE 1232
#define GTA6RT_X64_EXCEPTION_MAX_PARAMS 15

typedef struct gta6rt_x64_exception_record {
    uint32_t code;
    uint32_t flags;
    struct gta6rt_x64_exception_record* chained;
    uint64_t address;
    uint32_t n_params;
    uint32_t pad_;
    uint64_t params[GTA6RT_X64_EXCEPTION_MAX_PARAMS];
} gta6rt_x64_exception_record;

#define GTA6RT_X64_EXCEPTION_RECORD_SIZE 152
