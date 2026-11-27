#if !defined(__x86_64__)

#include <string.h>

#include <windows.h>

#include "seh_internal.h"

// arm64 host: guest x86-64 state lives inside the emulator, so the context capture/restore entry points that the
// x86_64 build implements in assembly cannot exist here. Table lookup, RtlVirtualUnwind, backtraces and vectored
// handlers work; frame-based dispatch aborts with a log (see dispatch_exception).

extern "C" {

void seh_restore_raw(const seh::ctx_t*) { seh::fatal("RtlRestoreContext: restoring a guest context is x86_64-only"); }

void seh_restore_raw_ymm(const seh::ctx_t*, const uint8_t*) {
    seh::fatal("RtlRestoreContext: restoring a guest context is x86_64-only");
}

void NTAPI RtlCaptureContext(PCONTEXT ctx) {
    if (!ctx) return;
    memset(ctx, 0, sizeof *ctx);
    ctx->ContextFlags = CONTEXT_FULL;
}

void WINAPI RaiseException(DWORD code, DWORD flags, DWORD count, const ULONG_PTR* args) {
    seh::rec_t r;
    memset(&r, 0, sizeof r);
    r.code = code;
    r.flags = flags & seh::kNonContinuable;
    if (count > GTA6RT_X64_EXCEPTION_MAX_PARAMS) count = GTA6RT_X64_EXCEPTION_MAX_PARAMS;
    if (args && count) {
        r.n_params = count;
        memcpy(r.params, args, count * sizeof(uint64_t));
    }
    alignas(16) seh::ctx_t c;
    memset(&c, 0, sizeof c);
    seh::dispatch_exception(&r, &c);
}

void NTAPI RtlRaiseException(PEXCEPTION_RECORD rec) {
    if (!rec) seh::fatal("RtlRaiseException(NULL)");
    alignas(16) seh::ctx_t c;
    memset(&c, 0, sizeof c);
    seh::dispatch_exception(reinterpret_cast<seh::rec_t*>(rec), &c);
}

void NTAPI RtlUnwindEx(PVOID, PVOID, PEXCEPTION_RECORD, PVOID, PCONTEXT, PUNWIND_HISTORY_TABLE) {
    seh::fatal("RtlUnwindEx is x86_64-only");
}

void NTAPI RtlUnwind(PVOID, PVOID, PEXCEPTION_RECORD, PVOID) { seh::fatal("RtlUnwind is x86_64-only"); }

WORD NTAPI RtlCaptureStackBackTrace(DWORD, DWORD, PVOID*, PDWORD hash) {
    if (hash) *hash = 0;
    return 0;
}

}  // extern "C"

#endif
