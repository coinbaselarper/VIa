#include <atomic>
#include <mutex>

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <windows.h>

#include <gta6rt/os.h>
#include <gta6rt/seh.h>

#include "seh_internal.h"

static_assert(sizeof(CONTEXT) == sizeof(gta6rt_x64_context), "CONTEXT layout");
static_assert(sizeof(EXCEPTION_RECORD) == sizeof(gta6rt_x64_exception_record), "EXCEPTION_RECORD layout");
static_assert(sizeof(DISPATCHER_CONTEXT) == sizeof(seh::dispatcher_context), "DISPATCHER_CONTEXT layout");
static_assert(sizeof(KNONVOLATILE_CONTEXT_POINTERS) == sizeof(seh::nonvol_ptrs), "KNONVOLATILE_CONTEXT_POINTERS layout");
static_assert(sizeof(RUNTIME_FUNCTION) == sizeof(gta6rt_runtime_function), "RUNTIME_FUNCTION layout");

namespace {

using seh::ctx_t;
using seh::rec_t;
using seh::rf_t;

std::once_flag g_init_once;

gta6rt_seh_policy env_policy(gta6rt_seh_policy def) {
    const char* e = getenv("GTA6RT_SEH_POLICY");
    if (!e || !*e) return def;
    if (!strcasecmp(e, "dispatch") || !strcmp(e, "1")) return GTA6RT_SEH_DISPATCH;
    if (!strcasecmp(e, "abort") || !strcasecmp(e, "log_and_abort") || !strcmp(e, "0")) return GTA6RT_SEH_LOG_AND_ABORT;
    seh::log("GTA6RT_SEH_POLICY=%s not understood (log_and_abort | dispatch); keeping %s", e,
             def == GTA6RT_SEH_DISPATCH ? "dispatch" : "log_and_abort");
    return def;
}

uint64_t image_base_any(uint64_t addr) {
    if (const uint64_t b = seh::image_base_of(addr)) return b;
    if (gta6rt_module* m = gta6rt_module_find_by_address(addr))
        if (const gta6rt_module_desc* d = gta6rt_module_info(m)) return d->base;
    return 0;
}

void fill_cxx_record(rec_t* r, uint64_t object, uint64_t throw_info, uint64_t address) {
    memset(r, 0, sizeof *r);
    r->code = seh::kCxxExceptionCode;
    r->flags = seh::kNonContinuable;
    r->address = address;
    r->n_params = 4;
    uint32_t attributes = 0;
    if (throw_info) seh::safe_read(throw_info, &attributes, 4);
    r->params[0] = (attributes & 8) ? 0x01994000u : 0x19930520u;
    r->params[1] = object;
    r->params[2] = throw_info;
    r->params[3] = throw_info ? image_base_any(throw_info) : 0;
}

}  // namespace

extern "C" {

gta6rt_status gta6rt_seh_init(gta6rt_seh_policy policy) {
    if (policy != GTA6RT_SEH_LOG_AND_ABORT && policy != GTA6RT_SEH_DISPATCH) return GTA6RT_E_INVALID_ARG;
    seh::set_policy(env_policy(policy));
    seh::init_blob();
    std::call_once(g_init_once, [] { seh::register_isa_fault_handler(); });
    if (seh::verbose())
        seh::log("init: policy %s", seh::policy() == GTA6RT_SEH_DISPATCH ? "DISPATCH" : "LOG_AND_ABORT");
    return GTA6RT_OK;
}

gta6rt_status gta6rt_seh_set_policy(gta6rt_seh_policy policy) {
    if (policy != GTA6RT_SEH_LOG_AND_ABORT && policy != GTA6RT_SEH_DISPATCH) return GTA6RT_E_INVALID_ARG;
    seh::set_policy(policy);
    return GTA6RT_OK;
}

gta6rt_seh_policy gta6rt_seh_get_policy(void) { return seh::policy(); }

gta6rt_status gta6rt_seh_add_function_table(uint64_t image_base, const gta6rt_runtime_function* table, uint32_t count,
                                            uint64_t image_size) {
    if (!table || !count) return GTA6RT_E_INVALID_ARG;
    seh::init_blob();
    return seh::table_add(image_base, table, count, image_size, image_size != 0) ? GTA6RT_OK : GTA6RT_E_NO_MEMORY;
}

gta6rt_status gta6rt_seh_remove_function_table(const gta6rt_runtime_function* table) {
    return seh::table_remove(table) ? GTA6RT_OK : GTA6RT_E_NOT_FOUND;
}

const gta6rt_runtime_function* gta6rt_seh_lookup(uint64_t pc, uint64_t* out_image_base) {
    uint64_t base = 0;
    const rf_t* f = seh::lookup(pc, &base);
    if (out_image_base) *out_image_base = f ? base : 0;
    return f;
}

gta6rt_status gta6rt_seh_virtual_unwind(uint32_t handler_type, uint64_t image_base, uint64_t pc,
                                        const gta6rt_runtime_function* fn, gta6rt_x64_context* ctx,
                                        void** out_handler_data, uint64_t* out_establisher_frame,
                                        uint64_t* out_handler) {
    if (!fn || !ctx) return GTA6RT_E_INVALID_ARG;
    seh::unwind_out o;
    if (!seh::virtual_unwind(handler_type, image_base, pc, fn, ctx, nullptr, false, &o)) return GTA6RT_E_BAD_FORMAT;
    if (out_handler_data) *out_handler_data = o.handler_data;
    if (out_establisher_frame) *out_establisher_frame = o.establisher;
    if (out_handler) *out_handler = o.handler;
    return GTA6RT_OK;
}

uint32_t gta6rt_seh_backtrace(const gta6rt_x64_context* ctx, uint64_t* pcs, uint32_t max) {
    return seh::backtrace(ctx, pcs, max);
}

gta6rt_status gta6rt_seh_dispatch(gta6rt_x64_exception_record* rec, gta6rt_x64_context* ctx) {
    if (!rec || !ctx) return GTA6RT_E_INVALID_ARG;
    seh::dispatch_exception(rec, ctx);
    return GTA6RT_OK;
}

void gta6rt_seh_raise(const gta6rt_x64_exception_record* rec, const gta6rt_x64_context* caller_ctx) {
    if (!rec || !caller_ctx) seh::fatal("gta6rt_seh_raise needs a record and the caller's context");
    alignas(16) ctx_t c;
    rec_t r;
    memcpy(&c, caller_ctx, sizeof c);
    memcpy(&r, rec, sizeof r);
    if (!r.address) r.address = c.rip;
    seh::dispatch_exception(&r, &c);
    if (r.flags & seh::kNonContinuable)
        seh::fatal_exception(&r, &c, "a noncontinuable exception was continued (STATUS_NONCONTINUABLE_EXCEPTION)", -2);
#if defined(__x86_64__)
    seh::restore_context(&c, nullptr);
#else
    seh::fatal("gta6rt_seh_raise: continuing a guest context is x86_64-only");
#endif
}

void gta6rt_seh_unwind(uint64_t target_frame, uint64_t target_ip, gta6rt_x64_exception_record* rec,
                       uint64_t return_value, gta6rt_x64_context* ctx) {
    if (!ctx) seh::fatal("gta6rt_seh_unwind needs the guest context to start from");
#if defined(__x86_64__)
    alignas(16) ctx_t start;
    memcpy(&start, ctx, sizeof start);
    seh::unwind(target_frame, target_ip, rec, return_value, ctx, nullptr, &start);
#else
    (void)target_frame;
    (void)target_ip;
    (void)rec;
    (void)return_value;
    seh::fatal("gta6rt_seh_unwind is x86_64-only");
#endif
}

gta6rt_status gta6rt_seh_add_vectored_handler(int first, gta6rt_gva handler, void** out_cookie) {
    if (!handler) return GTA6RT_E_INVALID_ARG;
    const uint64_t c = seh::add_vectored(false, first != 0, handler);
    if (!c) return GTA6RT_E_NO_MEMORY;
    if (out_cookie) *out_cookie = reinterpret_cast<void*>(c);
    return GTA6RT_OK;
}

gta6rt_status gta6rt_seh_remove_vectored_handler(void* cookie) {
    return seh::remove_vectored(false, reinterpret_cast<uint64_t>(cookie)) ? GTA6RT_OK : GTA6RT_E_NOT_FOUND;
}

gta6rt_gva gta6rt_seh_set_unhandled_filter(gta6rt_gva filter) { return seh::set_top_filter(filter); }

uint64_t gta6rt_seh_image_base_of(uint64_t addr) { return image_base_any(addr); }

// ---- Win32 surface (Windows x64 ABI on x86_64) ------------------------------------------------------------------

BOOLEAN NTAPI RtlAddFunctionTable(PRUNTIME_FUNCTION table, DWORD count, DWORD64 base) {
    seh::init_blob();
    return seh::table_add(base, reinterpret_cast<const rf_t*>(table), count, 0, false) ? TRUE : FALSE;
}

BOOLEAN NTAPI RtlDeleteFunctionTable(PRUNTIME_FUNCTION table) { return seh::table_remove(table) ? TRUE : FALSE; }

BOOLEAN NTAPI RtlInstallFunctionTableCallback(DWORD64 id, DWORD64 base, DWORD length, PGET_RUNTIME_FUNCTION_CALLBACK cb,
                                              PVOID context, PCWSTR) {
#if defined(__x86_64__)
    if ((id & 3) != 3) return FALSE;
    return seh::table_add_callback(id, base, length, reinterpret_cast<uint64_t>(cb), reinterpret_cast<uint64_t>(context))
               ? TRUE
               : FALSE;
#else
    (void)id;
    (void)base;
    (void)length;
    (void)cb;
    (void)context;
    return FALSE;
#endif
}

PRUNTIME_FUNCTION NTAPI RtlLookupFunctionEntry(DWORD64 pc, PDWORD64 image_base, PUNWIND_HISTORY_TABLE) {
    uint64_t base = 0;
    const rf_t* f = seh::lookup(pc, &base);
    if (image_base) *image_base = f ? base : 0;
    return reinterpret_cast<PRUNTIME_FUNCTION>(const_cast<rf_t*>(f));
}

PEXCEPTION_ROUTINE NTAPI RtlVirtualUnwind(DWORD type, DWORD64 base, DWORD64 pc, PRUNTIME_FUNCTION fn, PCONTEXT ctx,
                                          PVOID* handler_data, PDWORD64 establisher,
                                          PKNONVOLATILE_CONTEXT_POINTERS ptrs) {
    seh::unwind_out o;
    if (!fn || !ctx) return nullptr;
    if (!seh::virtual_unwind(type, base, pc, reinterpret_cast<const rf_t*>(fn), reinterpret_cast<ctx_t*>(ctx),
                             reinterpret_cast<seh::nonvol_ptrs*>(ptrs), false, &o)) {
        seh::log("RtlVirtualUnwind: bad unwind data for pc 0x%llx (image 0x%llx)", static_cast<unsigned long long>(pc),
                 static_cast<unsigned long long>(base));
        if (establisher) *establisher = o.establisher;
        return nullptr;
    }
    if (establisher) *establisher = o.establisher;
    if (o.handler && handler_data) *handler_data = o.handler_data;
    return reinterpret_cast<PEXCEPTION_ROUTINE>(o.handler);
}

void NTAPI RtlRestoreContext(PCONTEXT ctx, PEXCEPTION_RECORD rec) {
    seh::restore_context(reinterpret_cast<ctx_t*>(ctx), reinterpret_cast<rec_t*>(rec));
}

PVOID NTAPI RtlPcToFileHeader(PVOID pc, PVOID* base) {
    const uint64_t b = image_base_any(reinterpret_cast<uint64_t>(pc));
    if (base) *base = reinterpret_cast<PVOID>(b);
    return reinterpret_cast<PVOID>(b);
}

PVOID WINAPI AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler) {
    return reinterpret_cast<PVOID>(seh::add_vectored(false, first != 0, reinterpret_cast<uint64_t>(handler)));
}

ULONG WINAPI RemoveVectoredExceptionHandler(PVOID cookie) {
    return seh::remove_vectored(false, reinterpret_cast<uint64_t>(cookie)) ? 1 : 0;
}

PVOID WINAPI AddVectoredContinueHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler) {
    return reinterpret_cast<PVOID>(seh::add_vectored(true, first != 0, reinterpret_cast<uint64_t>(handler)));
}

ULONG WINAPI RemoveVectoredContinueHandler(PVOID cookie) {
    return seh::remove_vectored(true, reinterpret_cast<uint64_t>(cookie)) ? 1 : 0;
}

LPTOP_LEVEL_EXCEPTION_FILTER WINAPI SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
    return reinterpret_cast<LPTOP_LEVEL_EXCEPTION_FILTER>(seh::set_top_filter(reinterpret_cast<uint64_t>(filter)));
}

LONG WINAPI UnhandledExceptionFilter(struct _EXCEPTION_POINTERS* info) {
    return seh::unhandled_exception_filter(info ? reinterpret_cast<rec_t*>(info->ExceptionRecord) : nullptr,
                                           info ? reinterpret_cast<ctx_t*>(info->ContextRecord) : nullptr);
}

}  // extern "C"

#if defined(__x86_64__)

extern "C" {

void gta6rt_seh_CxxThrowException(void* object, void* throw_info);

void seh_raise_entry(uint32_t code, uint32_t flags, uint32_t count, const uint64_t* args, const uint64_t*, ctx_t* ctx) {
    rec_t r;
    memset(&r, 0, sizeof r);
    r.code = code;
    r.flags = flags & seh::kNonContinuable;
    r.address = ctx->rip;
    if (count > GTA6RT_X64_EXCEPTION_MAX_PARAMS) count = GTA6RT_X64_EXCEPTION_MAX_PARAMS;
    if (args && count) {
        r.n_params = count;
        memcpy(r.params, args, count * sizeof(uint64_t));
    }
    seh::set_cxx_fallback(false);
    seh::dispatch_exception(&r, ctx);
    if (r.flags & seh::kNonContinuable)
        seh::fatal_exception(&r, ctx, "a noncontinuable exception was continued (STATUS_NONCONTINUABLE_EXCEPTION)", -2);
}

void seh_rtl_raise_entry(rec_t* rec, ctx_t* ctx) {
    if (!rec) seh::fatal("RtlRaiseException(NULL)");
    rec->address = ctx->rip;
    seh::set_cxx_fallback(false);
    seh::dispatch_exception(rec, ctx);
    if (rec->flags & seh::kNonContinuable)
        seh::fatal_exception(rec, ctx, "a noncontinuable exception was continued (STATUS_NONCONTINUABLE_EXCEPTION)", -2);
}

[[noreturn]] void seh_unwind_ex_entry(uint64_t frame, uint64_t ip, rec_t* rec, uint64_t retval, const uint64_t* entry_rsp,
                                      ctx_t* start) {
    ctx_t* context = reinterpret_cast<ctx_t*>(entry_rsp[5]);
    void* history = reinterpret_cast<void*>(entry_rsp[6]);
    seh::unwind(frame, ip, rec, retval, context, history, start);
}

[[noreturn]] void seh_unwind_entry(uint64_t frame, uint64_t ip, rec_t* rec, uint64_t retval, ctx_t* start) {
    seh::unwind(frame, ip, rec, retval, nullptr, nullptr, start);
}

[[noreturn]] void seh_cxx_throw_entry(uint64_t object, uint64_t throw_info, ctx_t* ctx) {
    rec_t r;
    fill_cxx_record(&r, object, throw_info, ctx->rip);
    seh::set_cxx_fallback(true);
    seh::dispatch_exception(&r, ctx);
    seh::fatal_exception(&r, ctx, "a C++ exception was continued", 0);
}

uint16_t seh_capture_backtrace_entry(uint32_t skip, uint32_t count, void** trace, uint32_t* hash, ctx_t* ctx) {
    uint64_t pcs[128];
    const uint32_t n = seh::backtrace(ctx, pcs, 128);
    uint32_t got = 0, sum = 0;
    for (uint32_t i = skip; i < n && got < count && trace; ++i, ++got) {
        trace[got] = reinterpret_cast<void*>(pcs[i]);
        sum += static_cast<uint32_t>(pcs[i]);
    }
    if (hash) *hash = sum;
    return static_cast<uint16_t>(got);
}

}  // extern "C"

namespace {

std::atomic<uint32_t> g_fallback_logged{0};

EXCEPTION_DISPOSITION fallback_handler(uint32_t bit, const char* name, rec_t* rec, uint64_t frame) {
    if (!(g_fallback_logged.fetch_or(bit) & bit))
        seh::log("%s stand-in called (packaged vcruntime not mapped): code 0x%08x frame 0x%llx -> ContinueSearch",
                 name, rec ? rec->code : 0, static_cast<unsigned long long>(frame));
    return ExceptionContinueSearch;
}

__attribute__((ms_abi)) EXCEPTION_DISPOSITION fallback_fh3(rec_t* rec, uint64_t frame, void*, void*) {
    return fallback_handler(1, "__CxxFrameHandler3", rec, frame);
}
__attribute__((ms_abi)) EXCEPTION_DISPOSITION fallback_fh4(rec_t* rec, uint64_t frame, void*, void*) {
    return fallback_handler(2, "__CxxFrameHandler4", rec, frame);
}
__attribute__((ms_abi)) EXCEPTION_DISPOSITION fallback_c_specific(rec_t* rec, uint64_t frame, void*, void*) {
    return fallback_handler(4, "__C_specific_handler", rec, frame);
}

struct export_entry {
    const char* name;
    uint64_t addr;
};

uint64_t find_export(const export_entry* t, size_t n, const char* name) {
    if (!name) return 0;
    for (size_t i = 0; i < n; ++i)
        if (!strcmp(t[i].name, name)) return t[i].addr;
    return 0;
}

#define SEH_EXPORT(fn) {#fn, reinterpret_cast<uint64_t>(&fn)}

}  // namespace

extern "C" uint64_t gta6rt_seh_native_export(const char* name) {
    static const export_entry table[] = {
        SEH_EXPORT(RtlCaptureContext),
        SEH_EXPORT(RtlLookupFunctionEntry),
        SEH_EXPORT(RtlVirtualUnwind),
        SEH_EXPORT(RtlUnwindEx),
        SEH_EXPORT(RtlUnwind),
        SEH_EXPORT(RtlPcToFileHeader),
        SEH_EXPORT(RtlAddFunctionTable),
        SEH_EXPORT(RtlDeleteFunctionTable),
        SEH_EXPORT(RtlInstallFunctionTableCallback),
        SEH_EXPORT(RtlRestoreContext),
        SEH_EXPORT(RtlRaiseException),
        SEH_EXPORT(RtlCaptureStackBackTrace),
        SEH_EXPORT(RaiseException),
        SEH_EXPORT(AddVectoredExceptionHandler),
        SEH_EXPORT(RemoveVectoredExceptionHandler),
        SEH_EXPORT(AddVectoredContinueHandler),
        SEH_EXPORT(RemoveVectoredContinueHandler),
        SEH_EXPORT(SetUnhandledExceptionFilter),
        SEH_EXPORT(UnhandledExceptionFilter),
    };
    return find_export(table, sizeof table / sizeof table[0], name);
}

extern "C" uint64_t gta6rt_seh_vcruntime_fallback(const char* name) {
    static const export_entry table[] = {
        {"_CxxThrowException", reinterpret_cast<uint64_t>(&gta6rt_seh_CxxThrowException)},
        {"__CxxFrameHandler3", reinterpret_cast<uint64_t>(&fallback_fh3)},
        {"__CxxFrameHandler4", reinterpret_cast<uint64_t>(&fallback_fh4)},
        {"__C_specific_handler", reinterpret_cast<uint64_t>(&fallback_c_specific)},
    };
    return find_export(table, sizeof table / sizeof table[0], name);
}

#else

extern "C" uint64_t gta6rt_seh_native_export(const char*) { return 0; }
extern "C" uint64_t gta6rt_seh_vcruntime_fallback(const char*) { return 0; }

#endif
