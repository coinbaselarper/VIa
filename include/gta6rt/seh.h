#pragma once

/*
 * gta6rt_seh: Windows x64 structured exception handling over .pdata (S1.3). Implemented by seh/.
 * Decision (NOTES.md S0.6): own .pdata unwinder and dispatcher (Wine's model). Step 1 is
 * GTA6RT_SEH_LOG_AND_ABORT; step 2 is full dispatch with the packaged vcruntime140 mapped unchanged.
 * Consumed by loader/ (table registration), isa/ via the fault handler seh registers, shims_core/
 * (RaiseException, AddVectoredExceptionHandler, RtlCaptureContext exports), host/.
 */

#include <gta6rt/abi.h>
#include <gta6rt/x64_state.h>

GTA6RT_API_BEGIN

#define GTA6RT_SEH_API_VERSION 2

typedef enum gta6rt_seh_policy {
    GTA6RT_SEH_LOG_AND_ABORT = 0, /* log code, site, thrown C++ type and a stack walk, then abort */
    GTA6RT_SEH_DISPATCH = 1       /* run vectored handlers and language handlers, unwind */
} gta6rt_seh_policy;

/* RUNTIME_FUNCTION as stored in .pdata (12 bytes, RVAs). */
typedef struct gta6rt_runtime_function {
    uint32_t begin_rva;
    uint32_t end_rva;
    uint32_t unwind_info_rva;
} gta6rt_runtime_function;

/* Registers seh's handler with isa (gta6rt_isa_set_guest_fault_handler). Call after gta6rt_isa_init.
 * GTA6RT_SEH_POLICY=log_and_abort|dispatch in the environment overrides policy; GTA6RT_SEH_VERBOSE=1 traces. */
gta6rt_status gta6rt_seh_init(gta6rt_seh_policy policy);
gta6rt_status gta6rt_seh_set_policy(gta6rt_seh_policy policy);

/* Function tables (RtlAddFunctionTable / RtlDeleteFunctionTable). The table must stay mapped. */
gta6rt_status gta6rt_seh_add_function_table(uint64_t image_base, const gta6rt_runtime_function* table, uint32_t count,
                                            uint64_t image_size);
gta6rt_status gta6rt_seh_remove_function_table(const gta6rt_runtime_function* table);
/* RtlLookupFunctionEntry. NULL for leaf code. */
const gta6rt_runtime_function* gta6rt_seh_lookup(uint64_t pc, uint64_t* out_image_base);

/* RtlVirtualUnwind: unwinds ctx by one frame. handler_type is UNW_FLAG_EHANDLER/UHANDLER or 0.
 * Returns the language handler address (0 if none) through out_handler. */
gta6rt_status gta6rt_seh_virtual_unwind(uint32_t handler_type, uint64_t image_base, uint64_t pc,
                                        const gta6rt_runtime_function* fn, gta6rt_x64_context* ctx,
                                        void** out_handler_data, uint64_t* out_establisher_frame,
                                        uint64_t* out_handler);
/* Walk up to max frames from ctx (ctx is not modified). Returns the number of PCs written. */
uint32_t gta6rt_seh_backtrace(const gta6rt_x64_context* ctx, uint64_t* pcs, uint32_t max);

/* Dispatch an exception raised in guest code. Returns GTA6RT_OK if a handler asked to continue execution
 * (ctx updated), or does not return (unwind to a handler / abort under LOG_AND_ABORT). */
gta6rt_status gta6rt_seh_dispatch(gta6rt_x64_exception_record* rec, gta6rt_x64_context* ctx);
/* RaiseException / RtlRaiseException for a shim that has the guest caller's context (e.g. from a FRAME thunk):
 * dispatches; if a handler continues execution, resumes caller_ctx (x86_64; arm64 aborts). Does not return.
 * On x86_64 guests should call the native exports (gta6rt_seh_native_export), which capture the context. */
void gta6rt_seh_raise(const gta6rt_x64_exception_record* rec, const gta6rt_x64_context* caller_ctx);
/* RtlUnwindEx for host callers: ctx holds, on input, the guest context to start unwinding from. Does not return
 * (restores the target context). x86_64 only. */
void gta6rt_seh_unwind(uint64_t target_frame, uint64_t target_ip, gta6rt_x64_exception_record* rec, uint64_t return_value,
                       gta6rt_x64_context* ctx);

/* Vectored handlers (AddVectoredExceptionHandler). handler is a guest address (Win64 ABI). */
gta6rt_status gta6rt_seh_add_vectored_handler(int first, gta6rt_gva handler, void** out_cookie);
gta6rt_status gta6rt_seh_remove_vectored_handler(void* cookie);
gta6rt_gva gta6rt_seh_set_unhandled_filter(gta6rt_gva filter);

/* ---- API version 2 additions ------------------------------------------------------------------------- */

gta6rt_seh_policy gta6rt_seh_get_policy(void);

/* Address of seh's Win64-ABI implementation of a Windows export, for binding guest imports (x86_64 only; 0 on
 * arm64 or for names seh does not implement). Covers RtlCaptureContext, RtlLookupFunctionEntry, RtlVirtualUnwind,
 * RtlUnwindEx, RtlUnwind, RtlPcToFileHeader, RtlAddFunctionTable, RtlDeleteFunctionTable,
 * RtlInstallFunctionTableCallback, RtlRestoreContext, RtlRaiseException, RtlCaptureStackBackTrace, RaiseException,
 * Add/RemoveVectoredExceptionHandler, Add/RemoveVectoredContinueHandler, SetUnhandledExceptionFilter,
 * UnhandledExceptionFilter. Bind these directly (no frame-adding thunk): the capture/raise/unwind entries read
 * their caller's frame. */
uint64_t gta6rt_seh_native_export(const char* name);
/* Step-1 stand-ins for vcruntime140(_1).dll when the packaged DLLs are not mapped: _CxxThrowException (logs the
 * thrown type, then dispatches under the current policy) and __CxxFrameHandler3, __CxxFrameHandler4,
 * __C_specific_handler (log once, return ExceptionContinueSearch). 0 for other names or on arm64. */
uint64_t gta6rt_seh_vcruntime_fallback(const char* name);

/* RtlPcToFileHeader for host callers: image base of the registered image containing addr, or 0. */
uint64_t gta6rt_seh_image_base_of(uint64_t addr);

/* Hardware faults. Call from a SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGTRAP handler (after
 * gta6rt_isa_enter_host_from_signal) running on a sigaltstack. If the faulting pc is in a registered guest image,
 * converts the Darwin x86_64 ucontext to EXCEPTION_RECORD + CONTEXT (keeping the YMM upper halves), copies them
 * onto the faulting thread's stack and rewrites the ucontext so dispatch runs once the handler returns; returns 1.
 * Returns 0 for faults outside guest code (chain to the previous handler). x86_64 only (0 on arm64). */
int gta6rt_seh_handle_signal(int sig, void* siginfo, void* ucontext);
/* The conversions it uses, for isa/: Darwin x86_64 ucontext_t <-> CONTEXT, and signal -> EXCEPTION_RECORD
 * (int3 moves ctx->rip back onto the int3; int 0x29 becomes the fastfail STATUS_STACK_BUFFER_OVERRUN).
 * GTA6RT_E_UNSUPPORTED on arm64. */
gta6rt_status gta6rt_seh_context_from_ucontext(const void* ucontext, gta6rt_x64_context* ctx);
gta6rt_status gta6rt_seh_context_to_ucontext(const gta6rt_x64_context* ctx, void* ucontext);
gta6rt_status gta6rt_seh_record_from_signal(int sig, const void* siginfo, const void* ucontext, gta6rt_x64_context* ctx,
                                            gta6rt_x64_exception_record* rec);

GTA6RT_API_END
