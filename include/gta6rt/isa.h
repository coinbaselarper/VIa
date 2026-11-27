#pragma once

/*
 * gta6rt_isa: everything that depends on how guest x86-64 code executes (S1.2, X2/X5).
 * Backends: isa/rosetta (x86_64 host, guest runs natively under Rosetta) and isa/fexcore (arm64 host,
 * guest runs in libFEXCore). This is the only interface allowed to hide Rosetta- or FEX-specific code.
 * Consumed by loader/, seh/, shims_core/, host/.
 *
 * TEB model is SHARE (NOTES.md S0.6): TEB/TLS/PEB pointers are planted in the thread's own TSD slots so
 * gs:[0x30]/[0x58]/[0x60] work without repointing GS. SWAP stays available behind the same calls
 * (gta6rt_isa_config.teb_mode). Host signal handlers entered during guest code must call
 * gta6rt_isa_enter_host_from_signal() first (S0.1: handlers inherit the guest GS under SWAP).
 */

#include <gta6rt/abi.h>
#include <gta6rt/x64_state.h>

GTA6RT_API_BEGIN

#define GTA6RT_ISA_API_VERSION 7 /* 2: TLS images/slots, guest code ranges, image fixups, foreign threads;
                                    3: advertised features, re-exec for Rosetta's cpuid;
                                    4: GTA6RT_ISA_FIXUP_ZEN and its report fields;
                                    5: MONITORX/MWAITX wait emulation (zen_wait), monitor wake, vshift check;
                                    6: NATIVE thunks with a signature (arm64/fexcore marshalling);
                                    7: guest context capture/resume inside a thunk (fexcore) */

typedef enum gta6rt_isa_backend { GTA6RT_ISA_ROSETTA = 1, GTA6RT_ISA_FEXCORE = 2 } gta6rt_isa_backend;
typedef enum gta6rt_teb_mode { GTA6RT_TEB_SHARE = 0, GTA6RT_TEB_SWAP = 1 } gta6rt_teb_mode;

/* CPUID features the guest may see (S0.6 / S0.2). Default policy advertises exactly GTA6RT_CPUID_DEFAULT. */
#define GTA6RT_CPUID_AVX (1ull << 0)
#define GTA6RT_CPUID_AVX2 (1ull << 1)
#define GTA6RT_CPUID_FMA3 (1ull << 2)
#define GTA6RT_CPUID_F16C (1ull << 3)
#define GTA6RT_CPUID_BMI1 (1ull << 4)
#define GTA6RT_CPUID_BMI2 (1ull << 5)
#define GTA6RT_CPUID_MOVBE (1ull << 6)
#define GTA6RT_CPUID_LZCNT (1ull << 7)
#define GTA6RT_CPUID_SSE4A (1ull << 8)   /* never advertised: Rosetta SIGILLs insertq/extrq */
#define GTA6RT_CPUID_ADX (1ull << 9)     /* never advertised */
#define GTA6RT_CPUID_SHA (1ull << 10)    /* never advertised */
#define GTA6RT_CPUID_VAES (1ull << 11)   /* never advertised */
#define GTA6RT_CPUID_AVX512 (1ull << 12) /* never advertised */
#define GTA6RT_CPUID_DEFAULT                                                                                  \
    (GTA6RT_CPUID_AVX | GTA6RT_CPUID_AVX2 | GTA6RT_CPUID_FMA3 | GTA6RT_CPUID_F16C | GTA6RT_CPUID_BMI1 | \
     GTA6RT_CPUID_BMI2 | GTA6RT_CPUID_MOVBE | GTA6RT_CPUID_LZCNT)

typedef struct gta6rt_isa_config {
    gta6rt_teb_mode teb_mode;
    uint64_t cpuid_features; /* 0 = GTA6RT_CPUID_DEFAULT */
} gta6rt_isa_config;

typedef struct gta6rt_cpuid_regs {
    uint32_t eax, ebx, ecx, edx;
} gta6rt_cpuid_regs;

gta6rt_isa_backend gta6rt_isa_backend_id(void);
const char* gta6rt_isa_backend_name(void); /* "rosetta" / "fexcore" */
gta6rt_status gta6rt_isa_init(const gta6rt_isa_config* cfg);

/* What the guest gets for cpuid(leaf, subleaf) under the active policy. Under Rosetta the guest's own
 * cpuid instruction is answered by Rosetta, so this is enforced only where isa/ can intercept it;
 * gta6rt_isa_cpuid_enforced() says whether it is. */
gta6rt_status gta6rt_isa_cpuid(uint32_t leaf, uint32_t subleaf, gta6rt_cpuid_regs* out);
int gta6rt_isa_cpuid_enforced(void);
/* Host features actually available to guest code (Rosetta/FEX), for the startup preflight. */
uint64_t gta6rt_isa_host_features(void);

/* ---- per-thread TEB ------------------------------------------------------ */

typedef struct gta6rt_teb_desc {
    uint64_t stack_base;   /* highest address (TEB+0x08) */
    uint64_t stack_limit;  /* lowest committed address (TEB+0x10) */
    void* tls_pointer;     /* ThreadLocalStoragePointer (TEB+0x58): array indexed by _tls_index */
    void* peb;             /* TEB+0x60, process-wide; see gta6rt_isa_set_peb */
    uint32_t process_id;
    uint32_t thread_id;
} gta6rt_teb_desc;

/* Process PEB (built by loader/ or host/; layout in nt_layout.h). Must be set before the first attach. */
gta6rt_status gta6rt_isa_set_peb(void* peb);
/* Allocate and install a TEB on the calling thread. Idempotent per thread. */
gta6rt_status gta6rt_isa_thread_attach(const gta6rt_teb_desc* desc, void** out_teb);
void gta6rt_isa_thread_detach(void);
/* Host-safe accessors (never read gs: from portable code). NULL if the thread has no TEB. */
void* gta6rt_isa_current_teb(void);
gta6rt_status gta6rt_isa_set_tls_pointer(void* tls_pointer);
/* Signal-handler prologue/epilogue: switch to host TSD if needed, return a token for leave. */
uint64_t gta6rt_isa_enter_host_from_signal(void);
void gta6rt_isa_leave_host_to_signal(uint64_t token);

/* ---- host -> guest calls -------------------------------------------------- */

/* Win64 call: up to 16 args; bit i of float_mask means arg i travels in xmm (low 64 bits of int_args[i]).
 * The callee gets 32 bytes of shadow space and a 16-byte aligned stack (S0.3 bugs #5/#6). */
typedef struct gta6rt_guest_args {
    uint64_t args[16];
    uint32_t count;
    uint32_t float_mask;
} gta6rt_guest_args;

typedef struct gta6rt_guest_ret {
    uint64_t rax;
    uint64_t xmm0_lo;
} gta6rt_guest_ret;

gta6rt_status gta6rt_isa_call_guest(gta6rt_gva target, const gta6rt_guest_args* args, gta6rt_guest_ret* out);
uint64_t gta6rt_isa_call_guest4(gta6rt_gva target, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
/* Run an image entry point (exe entry, DllMain) on the current thread's guest stack. */
gta6rt_status gta6rt_isa_run_entry(gta6rt_gva entry, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t* out_rax);

/* ---- guest -> host thunks -------------------------------------------------- */

/* Guest register state at a FRAME-kind thunk. rsp points at the guest return address; stack args start at
 * rsp+0x28. The handler writes rax (and xmm0 for float returns) into the frame. */
typedef struct gta6rt_guest_frame {
    uint64_t rcx, rdx, r8, r9;
    uint64_t xmm0, xmm1, xmm2, xmm3; /* low 64 bits */
    uint64_t rsp;
    uint64_t rax;
    uint64_t xmm0_ret;
    uint32_t ret_is_float;
    uint32_t reserved;
} gta6rt_guest_frame;

typedef void (*gta6rt_frame_handler)(gta6rt_guest_frame* f, void* user);

typedef enum gta6rt_thunk_kind {
    GTA6RT_THUNK_NATIVE = 1, /* fn is a GTA6RT_GUEST_ABI function; x86_64/Rosetta only */
    GTA6RT_THUNK_FRAME = 2,  /* fn is a gta6rt_frame_handler; both backends */
    GTA6RT_THUNK_TRAP = 3    /* unresolved import: logs name and calls fn (a gta6rt_frame_handler) or aborts */
} gta6rt_thunk_kind;

typedef struct gta6rt_thunk_desc {
    gta6rt_thunk_kind kind;
    const void* fn;
    void* user;
    const char* debug_name; /* "kernel32.dll!CreateEventW"; kept for logs */
} gta6rt_thunk_desc;

/* Returns a guest-callable address (stable for the process lifetime). */
gta6rt_status gta6rt_isa_make_thunk(const gta6rt_thunk_desc* desc, gta6rt_gva* out);

/* ---- faults ---------------------------------------------------------------- */

typedef enum gta6rt_fault_action {
    GTA6RT_FAULT_CONTINUE_SEARCH = 0, /* not handled: next handler / default crash */
    GTA6RT_FAULT_RESUME = 1           /* ctx was updated; resume the guest with it */
} gta6rt_fault_action;

/* Called for synchronous faults in guest code, with the guest state as a Windows CONTEXT and the fault as an
 * EXCEPTION_RECORD (code = EXCEPTION_ACCESS_VIOLATION, ILLEGAL_INSTRUCTION, ...). seh/ registers this. */
typedef gta6rt_fault_action (*gta6rt_guest_fault_handler)(gta6rt_x64_exception_record* rec, gta6rt_x64_context* ctx,
                                                           void* user);
gta6rt_status gta6rt_isa_set_guest_fault_handler(gta6rt_guest_fault_handler fn, void* user);

/* Illegal-instruction hook, tried before the fault handler (S0.6: SSE4a insertq/extrq emulation).
 * Return GTA6RT_FAULT_RESUME after emulating and advancing ctx->rip. */
typedef gta6rt_fault_action (*gta6rt_illegal_insn_hook)(gta6rt_x64_context* ctx, const uint8_t* insn, void* user);
gta6rt_status gta6rt_isa_set_illegal_instruction_hook(gta6rt_illegal_insn_hook fn, void* user);

/* Code patching with cache maintenance (e.g. __chkstk -> ret at load, S0.6). Not safe on live code: the bytes
 * are not written atomically, and on a page that cannot be made RWX the page loses execute permission while it
 * is written. Use it only before the code can run on any thread. Thunks and patch stubs live in arenas that stay
 * executable and are safe to extend while other threads run. */
gta6rt_status gta6rt_isa_patch_code(gta6rt_gva addr, const void* bytes, size_t size);
void gta6rt_isa_flush_icache(gta6rt_gva addr, size_t size);

/* ==== API 2 (additive) ====================================================== */

/* Mode actually used by the calling thread (SHARE falls back to SWAP per thread when TSD slots 11/12 are
 * already taken; slot 6 must be free in both modes). -1 if the thread has no TEB. */
int gta6rt_isa_thread_teb_mode(void);

/* ---- implicit TLS (gs:[0x58]) and TlsAlloc slots ---------------------------- */

/* Per-thread block for one TLS image (copy of the template + zero fill). Called on the registering thread for
 * every attached thread, and on the attaching thread for later threads. */
typedef void* (*gta6rt_tls_block_ctor)(uint32_t tls_index, uint32_t thread_id, void* user);
typedef void (*gta6rt_tls_block_dtor)(void* block, void* user);
/* Register an image's implicit TLS under its _tls_index. Grows the ThreadLocalStoragePointer array of every
 * attached thread (late-loaded DLLs); threads attached later with desc->tls_pointer == NULL get an array built
 * from all registered images. A non-NULL desc->tls_pointer is used as given for indices registered before the
 * attach, and is replaced by an isa-owned copy on the first growth. */
gta6rt_status gta6rt_isa_tls_register_image(uint32_t tls_index, gta6rt_tls_block_ctor ctor, gta6rt_tls_block_dtor dtor,
                                             void* user);
gta6rt_status gta6rt_isa_tls_unregister_image(uint32_t tls_index); /* frees the blocks of every thread */
/* TlsAlloc/TlsFree/TlsGetValue/TlsSetValue storage: TEB.TlsSlots (64) + TlsExpansionSlots (1024). */
#define GTA6RT_ISA_TLS_SLOTS_MAX (64 + 1024)
gta6rt_status gta6rt_isa_tls_slot_alloc(uint32_t* out_index);
gta6rt_status gta6rt_isa_tls_slot_free(uint32_t index); /* clears the slot in every thread (Win32 TlsFree) */
void* gta6rt_isa_tls_slot_get(uint32_t index);
gta6rt_status gta6rt_isa_tls_slot_set(uint32_t index, void* value);

/* Threads that call into the guest without a TEB (dispatch, CoreAudio, Metal callbacks) get one on the spot in
 * gta6rt_isa_call_guest/run_entry: through this hook if set (it must call gta6rt_isa_thread_attach), otherwise
 * with pthread stack bounds and TLS from the registered images. Such TEBs are freed at thread exit. */
typedef gta6rt_status (*gta6rt_foreign_thread_hook)(void* user);
gta6rt_status gta6rt_isa_set_foreign_thread_hook(gta6rt_foreign_thread_hook fn, void* user);

/* Faults whose pc is outside every registered range are passed to the previously installed handler.
 * With no range registered, faults on threads with a TEB are treated as guest faults. */
gta6rt_status gta6rt_isa_register_guest_code(gta6rt_gva base, size_t size);

/* ---- load-time fixups of the title image (S0.6 host obligations) ------------ */

#define GTA6RT_ISA_FIXUP_CHKSTK (1u << 0)      /* __chkstk -> ret (gs:[0x10] is libc's under SHARE) */
#define GTA6RT_ISA_FIXUP_SSE4A_PATCH (1u << 1) /* insertq/extrq sites -> call into emulation stubs */
#define GTA6RT_ISA_FIXUP_VSHIFT (1u << 2)      /* vpsrlvq/vpsllvq sites not proven safe -> fixed stubs */
#define GTA6RT_ISA_FIXUP_CPUID (1u << 3)       /* cpuid/xgetbv sites -> int3 trap answered by the policy */
#define GTA6RT_ISA_FIXUP_ALL 0xfu
#define GTA6RT_ISA_FIXUP_ZEN (1u << 4) /* API 4: CLWB/MONITORX/MWAITX -> NOP at the title's sites, CLZERO loops -> stubs */

typedef struct gta6rt_isa_fixup_report {
    uint32_t known_image; /* 1 if the image matched a built-in site table (by bytes at every site) */
    uint32_t chkstk, sse4a, vshift, cpuid, xgetbv; /* sites patched */
    uint32_t mismatched; /* sites whose bytes did not match the table (left untouched) */
    uint32_t vshift_safe; /* sites left alone because their count is proven < 2^32 */
    uint32_t zen_nop;      /* API 4: CLWB/MONITORX/MWAITX sites made NOPs */
    uint32_t clzero_loops; /* API 4: CLZERO loops moved to arena stubs */
    uint32_t zen_wait;     /* API 5: MONITORX/MWAITX sites calling the host wait emulation (not counted in zen_nop) */
    uint32_t vshift_checked; /* API 5: vshift sites under the run-time check (GTA6RT_ISA_VSHIFT_CHECK=1) */
} gta6rt_isa_fixup_report;

/* Apply the fixups for a mapped image (sections at their RVAs, before it runs). Unknown images get only the
 * __chkstk scan. SSE4a sites that are not patched are still handled by the SIGILL emulator. */
gta6rt_status gta6rt_isa_apply_image_fixups(gta6rt_gva image_base, size_t image_size, uint32_t flags,
                                             gta6rt_isa_fixup_report* out);
/* Building blocks, also usable on other images. Each verifies the instruction bytes before writing. */
gta6rt_gva gta6rt_isa_find_chkstk(gta6rt_gva base, size_t size); /* 0 if not found */
gta6rt_status gta6rt_isa_patch_chkstk(gta6rt_gva fn);
gta6rt_status gta6rt_isa_patch_sse4a_site(gta6rt_gva site);   /* decoded insertq/extrq, >= 5 bytes */
gta6rt_status gta6rt_isa_patch_vshift_site(gta6rt_gva site);  /* vpsrlvq/vpsllvq, register or memory count */
gta6rt_status gta6rt_isa_patch_cpuid_site(gta6rt_gva site);   /* cpuid (0f a2) or xgetbv (0f 01 d0) */
/* Emulated SSE4a traps and trapped cpuid/xgetbv executions since start (diagnostics). */
uint64_t gta6rt_isa_trap_count(void);

/* ==== API 3 (additive) ====================================================== */

/* Features the guest's own cpuid instruction reports (Rosetta's answer on x86_64), as GTA6RT_CPUID_* bits.
 * Rosetta advertises exactly GTA6RT_CPUID_DEFAULT (plus RDRAND) only if ROSETTA_ADVERTISE_AVX=1 was in the
 * environment when the process started; without it, no AVX/AVX2/FMA/F16C/BMI/MOVBE. Setting it later has no
 * effect (measured). gta6rt_isa_cpuid_enforced() is 1 when this equals the policy or the image sites are trapped. */
uint64_t gta6rt_isa_advertised_features(void);
/* Call first in main(). If the policy needs AVX and Rosetta does not advertise it, sets ROSETTA_ADVERTISE_AVX=1 and
 * re-executes the current executable with argv (no return on success), so packaged DLLs, which are not patched,
 * see the policy too. Returns OK when nothing is needed (already advertised, not translated, fexcore), and
 * GTA6RT_E_UNSUPPORTED if a re-executed process still does not see it. */
gta6rt_status gta6rt_isa_reexec_for_cpuid(char** argv);

/* ==== API 5 (additive) ====================================================== */
/* Host code that stores to memory a guest thread may be waiting on with MONITORX/MWAITX (the D3D12 fence mirror
 * behind fence slot 12) calls this after the store; the emulated MWAITX then returns at once instead of at its next
 * poll step. addr must be 8-byte aligned; no-op otherwise. Safe on any thread, both backends. */
void gta6rt_isa_monitor_wake(const void* addr);
/* Logs the vpsrlvq/vpsllvq run-time check counters (no-op unless GTA6RT_ISA_VSHIFT_CHECK=1 was set at fixup). */
void gta6rt_isa_vshift_check_report(void);

/* ==== API 6 (additive) ====================================================== */
/* A host function with a C signature called from guest code. On arm64 (fexcore) the guest's Win64 call is re-marshalled
 * into an AAPCS64 call of desc->fn (an ordinary host function): GPR and xmm argument positions, stack arguments from
 * rsp+0x28, sub-32-bit integers extended as Apple's ABI requires, Darwin's packed stack arguments, variadic arguments
 * on the stack, aggregates by value or by reference on each side, the Win64 hidden return buffer and AAPCS64 x8 or
 * register returns. desc->kind must be GTA6RT_THUNK_NATIVE. On x86_64 (rosetta) fn must be GTA6RT_GUEST_ABI and the
 * signature is not needed: integrator, see isa/fexcore/STATUS.md (rosetta does not define this function yet). */
typedef enum gta6rt_abi_type {
    GTA6RT_ABI_VOID = 0, /* return type only */
    GTA6RT_ABI_I64 = 1,  /* 64-bit integer or pointer */
    GTA6RT_ABI_I32 = 2,
    GTA6RT_ABI_U32 = 3,
    GTA6RT_ABI_I16 = 4,
    GTA6RT_ABI_U16 = 5,
    GTA6RT_ABI_I8 = 6,
    GTA6RT_ABI_U8 = 7, /* also bool */
    GTA6RT_ABI_F32 = 8,
    GTA6RT_ABI_F64 = 9,
    GTA6RT_ABI_M128 = 10, /* __m128-class vector by value: Win64 passes a pointer to it, returns it in xmm0 */
    GTA6RT_ABI_AGG = 11   /* struct/union of `size` bytes: Win64 passes and returns 1/2/4/8-byte ones by value */
} gta6rt_abi_type;
#define GTA6RT_ABI_HFA_F64 0x10 /* gta6rt_abi_arg.hfa: members are doubles (else floats); low nibble = count 1..4 */
#define GTA6RT_ABI_NOT_VARIADIC 0xFF
typedef struct gta6rt_abi_arg {
    uint8_t type; /* gta6rt_abi_type */
    uint8_t hfa;  /* AGG made of 1..4 floats or doubles only (AAPCS64 HFA): count | GTA6RT_ABI_HFA_F64; 0 otherwise */
    uint16_t size; /* AGG only */
} gta6rt_abi_arg;
typedef struct gta6rt_native_sig {
    gta6rt_abi_arg ret;
    uint8_t count; /* arguments, at most 16 */
    uint8_t fixed; /* variadic functions: named arguments; GTA6RT_ABI_NOT_VARIADIC otherwise */
    uint8_t reserved[2];
    gta6rt_abi_arg args[16];
} gta6rt_native_sig;
gta6rt_status gta6rt_isa_make_native_thunk(const gta6rt_thunk_desc* desc, const gta6rt_native_sig* sig, gta6rt_gva* out);

/* ==== API 7 (additive) ====================================================== */
/* For seh/ on arm64, where guest registers live in FEX's state (x86_64 seh/ uses its own assembly and does not call
 * these; rosetta does not define them yet). Both are valid only inside a FRAME/NATIVE/TRAP thunk handler.
 * Capture: the guest state as of the return from the thunk call, as RtlCaptureContext reports it (rip = return
 * address, rsp = rsp + 8; volatile registers as at the call). GTA6RT_E_NOT_FOUND outside a thunk handler.
 * Resume: drops the host frames of the innermost thunk call and continues the guest with ctx (RtlRestoreContext, the
 * end of RtlUnwindEx). ctx->rsp must lie inside the current host->guest call; otherwise, or outside a thunk handler,
 * it logs and aborts. Does not return. */
gta6rt_status gta6rt_isa_capture_guest_context(gta6rt_x64_context* out);
void gta6rt_isa_resume_guest_context(const gta6rt_x64_context* ctx) __attribute__((noreturn));

GTA6RT_API_END
