#include <atomic>
#include <mutex>

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gta6rt/isa.h>
#include <gta6rt/os.h>

#include "seh_internal.h"

namespace seh {

namespace {

std::atomic<int> g_policy{GTA6RT_SEH_LOG_AND_ABORT};
std::atomic<int> g_verbose{-1};
std::atomic<uint64_t> g_top_filter{0};

struct veh_entry {
    uint64_t handler;
    uint64_t cookie;
    bool cont;
};
std::mutex g_veh_mutex;
veh_entry g_veh[128];
int g_veh_count;
uint64_t g_cookie_next = 0x5e0000ull;

struct thread_state {
    uint64_t active_sp[40];
    int active;
    int in_fatal;
    int in_filter;
    int cxx_fallback;
};
thread_local thread_state t_state;

struct stack_bounds {
    uint64_t lo, hi;
    bool known;
};

stack_bounds current_stack() {
    const uint64_t sp = reinterpret_cast<uint64_t>(__builtin_frame_address(0));
    if (void* teb = gta6rt_isa_current_teb()) {
        uint64_t base, limit;
        memcpy(&base, static_cast<char*>(teb) + 8, 8);
        memcpy(&limit, static_cast<char*>(teb) + 16, 8);
        if (limit < base && sp >= limit && sp < base) return {limit, base, true};
    }
    pthread_t self = pthread_self();
    const uint64_t hi = reinterpret_cast<uint64_t>(pthread_get_stackaddr_np(self));
    const uint64_t lo = hi - pthread_get_stacksize_np(self);
    if (sp >= lo && sp < hi) return {lo, hi, true};
    return {0, ~0ull, false};
}

bool valid_frame(uint64_t frame, const stack_bounds& sb) {
    if (frame & 7) return false;
    return !sb.known || (frame >= sb.lo && frame <= sb.hi);
}

struct active_guard {
    bool pushed = false;
    active_guard() {
        const uint64_t sp = reinterpret_cast<uint64_t>(__builtin_frame_address(0));
        while (t_state.active && t_state.active_sp[t_state.active - 1] <= sp) --t_state.active;
        if (t_state.active >= 32) fatal("exception dispatch nested more than 32 deep");
        t_state.active_sp[t_state.active++] = sp;
        pushed = true;
    }
    ~active_guard() {
        if (pushed && t_state.active) --t_state.active;
    }
};

const char* code_name(uint32_t c) {
    switch (c) {
    case 0xE06D7363u: return "C++ exception";
    case 0xC0000005u: return "EXCEPTION_ACCESS_VIOLATION";
    case 0xC000001Du: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case 0xC0000096u: return "EXCEPTION_PRIV_INSTRUCTION";
    case 0xC0000094u: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case 0xC0000095u: return "EXCEPTION_INT_OVERFLOW";
    case 0xC00000FDu: return "EXCEPTION_STACK_OVERFLOW";
    case 0xC0000409u: return "STATUS_STACK_BUFFER_OVERRUN (fastfail)";
    case 0xC0000420u: return "STATUS_ASSERTION_FAILURE";
    case 0xC0000025u: return "STATUS_NONCONTINUABLE_EXCEPTION";
    case 0xC0000026u: return "STATUS_INVALID_DISPOSITION";
    case 0xC0000027u: return "STATUS_UNWIND";
    case 0xC0000028u: return "STATUS_BAD_STACK";
    case 0xC0000029u: return "STATUS_INVALID_UNWIND_TARGET";
    case 0xC000008Cu: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case 0xC000008Du: return "EXCEPTION_FLT_DENORMAL_OPERAND";
    case 0xC000008Eu: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case 0xC000008Fu: return "EXCEPTION_FLT_INEXACT_RESULT";
    case 0xC0000090u: return "EXCEPTION_FLT_INVALID_OPERATION";
    case 0xC0000091u: return "EXCEPTION_FLT_OVERFLOW";
    case 0xC0000093u: return "EXCEPTION_FLT_UNDERFLOW";
    case 0x80000003u: return "EXCEPTION_BREAKPOINT";
    case 0x80000004u: return "EXCEPTION_SINGLE_STEP";
    case 0x80000001u: return "EXCEPTION_GUARD_PAGE";
    case 0x80000002u: return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case 0x80000026u: return "STATUS_LONGJUMP";
    case 0x80000029u: return "STATUS_UNWIND_CONSOLIDATE";
    case 0x406D1388u: return "MS_VC_EXCEPTION (SetThreadName)";
    case 0x40010006u: return "DBG_PRINTEXCEPTION_C";
    case 0x4001000Au: return "DBG_PRINTEXCEPTION_WIDE_C";
    default: return "";
    }
}

struct throw_info {
    uint32_t attributes;
    int32_t unwind, fwd_compat, catchable_array;
};
struct catchable_type {
    uint32_t properties;
    int32_t type;
    int32_t mdisp, pdisp, vdisp;
    int32_t size;
    int32_t copy;
};

void demangle_type(const char* raw, char* out, size_t size) {
    if (raw[0] == '.' && raw[1] == '?' && raw[2] == 'A' && (raw[3] == 'V' || raw[3] == 'U')) {
        const char* p = raw + 4;
        const char* parts[8];
        size_t lens[8];
        int n = 0;
        while (*p && *p != '@' && n < 8) {
            const char* s = p;
            while (*p && *p != '@') ++p;
            parts[n] = s;
            lens[n++] = static_cast<size_t>(p - s);
            if (*p == '@') ++p;
        }
        size_t used = 0;
        out[0] = 0;
        for (int i = n - 1; i >= 0 && used + lens[i] + 3 < size; --i) {
            memcpy(out + used, parts[i], lens[i]);
            used += lens[i];
            if (i) {
                memcpy(out + used, "::", 2);
                used += 2;
            }
        }
        out[used] = 0;
        if (n) return;
    }
    static const struct {
        const char* m;
        const char* n;
    } builtins[] = {{".H", "int"},          {".I", "unsigned int"},  {".J", "long"},        {".K", "unsigned long"},
                    {"._J", "__int64"},     {"._K", "unsigned __int64"}, {".D", "char"},    {".E", "unsigned char"},
                    {".F", "short"},        {".G", "unsigned short"}, {".M", "float"},       {".N", "double"},
                    {".PEAD", "char *"},    {".PEBD", "const char *"}, {".PEA_W", "wchar_t *"}, {".PEB_W", "const wchar_t *"},
                    {".PEAX", "void *"},    {"._N", "bool"}};
    for (const auto& b : builtins)
        if (!strcmp(raw, b.m)) {
            snprintf(out, size, "%s", b.n);
            return;
        }
    snprintf(out, size, "%s", raw);
}

bool read_cstr(uint64_t addr, char* out, size_t size) {
    out[0] = 0;
    size_t i = 0;
    while (i + 1 < size) {
        char ch;
        if (!safe_read(addr + i, &ch, 1)) break;
        out[i++] = ch;
        if (!ch) return true;
    }
    out[i] = 0;
    return i > 0;
}

void describe_cxx(const rec_t* rec) {
    if (rec->code != kCxxExceptionCode || rec->n_params < 3) return;
    const uint64_t magic = rec->params[0], obj = rec->params[1], ti = rec->params[2];
    uint64_t base = rec->n_params >= 4 ? rec->params[3] : 0;
    if (!ti) {
        log("  C++ rethrow (throw;), magic 0x%llx: the rethrown type is not in the record",
            static_cast<unsigned long long>(magic));
        return;
    }
    throw_info t;
    if (!safe_read(ti, &t, sizeof t)) {
        log("  C++ ThrowInfo 0x%llx unreadable", static_cast<unsigned long long>(ti));
        return;
    }
    if (!base) base = image_base_of(ti);
    int32_t n = 0;
    if (!base || !safe_read(base + static_cast<uint32_t>(t.catchable_array), &n, 4) || n <= 0 || n > 64) {
        log("  C++ object 0x%llx ThrowInfo 0x%llx (catchable types unreadable, image base 0x%llx)",
            static_cast<unsigned long long>(obj), static_cast<unsigned long long>(ti),
            static_cast<unsigned long long>(base));
        return;
    }
    char names[512];
    size_t used = 0;
    names[0] = 0;
    char first[160] = "?";
    bool is_std_exception = false;
    for (int32_t i = 0; i < n && i < 12; ++i) {
        int32_t ct_rva = 0;
        catchable_type ct;
        char raw[160], nice[160];
        if (!safe_read(base + static_cast<uint32_t>(t.catchable_array) + 4 + 4 * i, &ct_rva, 4)) break;
        if (!safe_read(base + static_cast<uint32_t>(ct_rva), &ct, sizeof ct)) break;
        if (!read_cstr(base + static_cast<uint32_t>(ct.type) + 16, raw, sizeof raw)) break;
        demangle_type(raw, nice, sizeof nice);
        if (!strcmp(raw, ".?AVexception@std@@")) is_std_exception = true;
        if (!i) snprintf(first, sizeof first, "%s", nice);
        else {
            const int w = snprintf(names + used, sizeof names - used, "%s%s", used ? ", " : "", nice);
            if (w > 0) used += static_cast<size_t>(w) < sizeof names - used ? static_cast<size_t>(w) : 0;
        }
    }
    log("  C++ type %s%s%s%s object 0x%llx ThrowInfo 0x%llx magic 0x%llx", first, used ? " (catchable as: " : "",
        names, used ? ")" : "", static_cast<unsigned long long>(obj), static_cast<unsigned long long>(ti),
        static_cast<unsigned long long>(magic));
    if (is_std_exception && obj) {
        uint64_t what = 0;
        char msg[200];
        if (safe_read(obj + 8, &what, 8) && what && read_cstr(what, msg, sizeof msg)) log("  what(): \"%s\"", msg);
    }
}

#if defined(__x86_64__)
using host_veh = int32_t(__attribute__((ms_abi))*)(void*);
#else
using host_veh = int32_t (*)(void*);
#endif

struct exception_pointers {
    rec_t* rec;
    ctx_t* ctx;
};

int32_t call_filter_like(uint64_t fn, exception_pointers* ptrs, const ctx_t* frame_ctx) {
#if defined(__x86_64__)
    tramp_args a{};
    a.fn = fn;
    a.a[0] = reinterpret_cast<uint64_t>(ptrs);
    a.frame_ctx = frame_ctx;
    return static_cast<int32_t>(seh_tramp_plain(&a));
#else
    (void)frame_ctx;
    if (pc_in_guest_image(fn))
        return static_cast<int32_t>(gta6rt_isa_call_guest4(fn, reinterpret_cast<uint64_t>(ptrs), 0, 0, 0));
    return reinterpret_cast<host_veh>(fn)(ptrs);
#endif
}

bool informational(uint32_t code) { return code == 0x406D1388u || code == 0x40010006u || code == 0x4001000Au; }

int32_t call_vectored(bool cont, rec_t* rec, ctx_t* ctx) {
    uint64_t hs[128];
    int n = 0;
    {
        std::lock_guard<std::mutex> l(g_veh_mutex);
        for (int i = 0; i < g_veh_count; ++i)
            if (g_veh[i].cont == cont) hs[n++] = g_veh[i].handler;
    }
    exception_pointers ptrs{rec, ctx};
    for (int i = 0; i < n; ++i) {
        const int32_t r = call_filter_like(hs[i], &ptrs, ctx);
        if (verbose())
            log("%s handler 0x%llx returned %d", cont ? "vectored continue" : "vectored",
                static_cast<unsigned long long>(hs[i]), r);
        if (r == -1) return -1;
    }
    return 0;
}

bool dispatch_step(uint32_t type, dispatcher_context* d, ctx_t* c) {
    d->image_base = 0;
    d->scope_index = 0;
    d->control_pc = c->rip;
    d->language_handler = 0;
    d->handler_data = nullptr;
    d->function_entry = nullptr;
    if (!c->rip) {
        d->establisher_frame = 0;
        return true;
    }
    if (const rf_t* fn = lookup(c->rip, &d->image_base)) {
        d->function_entry = fn;
        unwind_out o;
        if (!virtual_unwind(type, d->image_base, c->rip, fn, c, nullptr, false, &o)) return false;
        d->language_handler = o.handler;
        d->handler_data = o.handler_data;
        d->establisher_frame = o.establisher;
        return true;
    }
    if (!pc_in_guest_image(c->rip)) {
        if (verbose()) log("walk left guest code at 0x%llx", static_cast<unsigned long long>(c->rip));
        d->establisher_frame = 0;
        return true;
    }
    d->establisher_frame = c->rsp;
    uint64_t ra;
    memcpy(&ra, reinterpret_cast<const void*>(c->rsp), 8);
    if (ra == c->rip) {
        d->establisher_frame = 0;
        return true;
    }
    c->rip = ra;
    c->rsp += 8;
    return true;
}

uint32_t call_handler(rec_t* rec, ctx_t* orig, dispatcher_context* d) {
    if (verbose())
        log("phase 1: handler 0x%llx frame 0x%llx pc 0x%llx", static_cast<unsigned long long>(d->language_handler),
            static_cast<unsigned long long>(d->establisher_frame), static_cast<unsigned long long>(d->control_pc));
#if defined(__x86_64__)
    tramp_args a{};
    a.fn = d->language_handler;
    a.a[0] = reinterpret_cast<uint64_t>(rec);
    a.a[1] = d->establisher_frame;
    a.a[2] = reinterpret_cast<uint64_t>(orig);
    a.a[3] = reinterpret_cast<uint64_t>(d);
    a.frame_ctx = orig;
    a.user = d;
    return static_cast<uint32_t>(seh_tramp_exception(&a));
#else
    (void)rec;
    (void)orig;
    return kDispContinueSearch;
#endif
}

uint32_t call_unwind_handler(rec_t* rec, dispatcher_context* d) {
    if (verbose())
        log("phase 2: handler 0x%llx frame 0x%llx pc 0x%llx flags 0x%x",
            static_cast<unsigned long long>(d->language_handler), static_cast<unsigned long long>(d->establisher_frame),
            static_cast<unsigned long long>(d->control_pc), rec->flags);
#if defined(__x86_64__)
    tramp_args a{};
    a.fn = d->language_handler;
    a.a[0] = reinterpret_cast<uint64_t>(rec);
    a.a[1] = d->establisher_frame;
    a.a[2] = reinterpret_cast<uint64_t>(d->context_record);
    a.a[3] = reinterpret_cast<uint64_t>(d);
    a.frame_ctx = d->context_record;
    a.user = d;
    return static_cast<uint32_t>(seh_tramp_unwind(&a));
#else
    (void)rec;
    return kDispContinueSearch;
#endif
}

uint32_t call_stack_handlers(rec_t* rec, ctx_t* orig) {
    const stack_bounds sb = current_stack();
    alignas(16) static thread_local uint8_t history[216];
    memset(history, 0, sizeof history);
    alignas(16) ctx_t context;
    memcpy(&context, orig, sizeof context);
    dispatcher_context d{};
    d.target_ip = 0;
    d.context_record = &context;
    d.history_table = history;
    uint32_t flags = rec->flags & kNonContinuable;
    uint64_t nested_frame = 0;
    bool skip_step = false;
    for (unsigned frames = 0; frames < 100000; ++frames) {
        if (!skip_step && !dispatch_step(kFlagEHandler, &d, &context)) {
            log("phase 1: unreadable unwind data at pc 0x%llx", static_cast<unsigned long long>(d.control_pc));
            break;
        }
        skip_step = false;
        if (!d.establisher_frame) break;
        if (!valid_frame(d.establisher_frame, sb)) {
            log("phase 1: invalid frame 0x%llx (stack 0x%llx-0x%llx)", static_cast<unsigned long long>(d.establisher_frame),
                static_cast<unsigned long long>(sb.lo), static_cast<unsigned long long>(sb.hi));
            flags |= kStackInvalid;
            break;
        }
        if (d.language_handler) {
            rec->flags = flags;
            const uint64_t frame = d.establisher_frame;
            const uint32_t r = call_handler(rec, orig, &d);
            flags |= rec->flags & kNonContinuable;
            if (nested_frame == frame) {
                flags &= ~kNestedCall;
                nested_frame = 0;
            }
            switch (r) {
            case kDispContinueExecution:
                rec->flags = flags;
                if (flags & kNonContinuable)
                    fatal_exception(rec, orig, "a handler continued a noncontinuable exception (STATUS_NONCONTINUABLE_EXCEPTION)", -1);
                return kStatusSuccess;
            case kDispContinueSearch: break;
            case kDispNested:
                flags |= kNestedCall;
                if (d.establisher_frame > nested_frame) nested_frame = d.establisher_frame;
                break;
            case kDispCollided: {
                memcpy(&context, d.context_record, sizeof context);
                d.context_record = &context;
                unwind_out o;
                if (!virtual_unwind(0, d.image_base, d.control_pc, d.function_entry, &context, nullptr, false, &o)) {
                    rec->flags = flags;
                    return kStatusInvalidDisposition;
                }
                skip_step = true;
                continue;
            }
            default: rec->flags = flags; return kStatusInvalidDisposition;
            }
        }
        if (sb.known && context.rsp >= sb.hi) break;
    }
    rec->flags = flags;
    return kStatusUnhandled;
}

int throw_frame_of(const rec_t* rec) {
    if (rec->code != kCxxExceptionCode) return 0;
    return t_state.cxx_fallback ? 0 : 1;
}

}  // namespace

bool verbose() {
    int v = g_verbose.load(std::memory_order_relaxed);
    if (v < 0) {
        const char* e = getenv("GTA6RT_SEH_VERBOSE");
        v = e && *e && *e != '0';
        g_verbose.store(v, std::memory_order_relaxed);
    }
    return v != 0;
}

gta6rt_seh_policy policy() { return static_cast<gta6rt_seh_policy>(g_policy.load(std::memory_order_relaxed)); }
void set_policy(gta6rt_seh_policy p) { g_policy.store(p, std::memory_order_relaxed); }
void set_cxx_fallback(bool on) { t_state.cxx_fallback = on; }

void log(const char* fmt, ...) {
    char buf[1200];
    memcpy(buf, "[seh] ", 6);
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + 6, sizeof buf - 8, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    size_t len = 6 + (static_cast<size_t>(n) < sizeof buf - 8 ? static_cast<size_t>(n) : sizeof buf - 9);
    buf[len++] = '\n';
    ssize_t w = write(2, buf, len);
    (void)w;
}

void fatal(const char* fmt, ...) {
    char buf[1000];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log("FATAL: %s", buf);
    abort();
}

void describe_address(uint64_t pc, char* buf, size_t size) {
    if (gta6rt_module* m = gta6rt_module_find_by_address(pc)) {
        const gta6rt_module_desc* d = gta6rt_module_info(m);
        if (d && d->name) {
            snprintf(buf, size, "0x%llx (%s+0x%llx)", static_cast<unsigned long long>(pc), d->name,
                     static_cast<unsigned long long>(pc - d->base));
            return;
        }
    }
    if (const uint64_t base = image_base_of(pc))
        snprintf(buf, size, "0x%llx (image 0x%llx+0x%llx)", static_cast<unsigned long long>(pc),
                 static_cast<unsigned long long>(base), static_cast<unsigned long long>(pc - base));
    else
        snprintf(buf, size, "0x%llx%s", static_cast<unsigned long long>(pc), pc_in_guest_image(pc) ? "" : " (not guest code)");
}

void fatal_exception(const rec_t* rec, const ctx_t* ctx, const char* why, int throw_frame) {
    if (t_state.in_fatal++) {
        static const char msg[] = "[seh] FATAL: exception while reporting an exception\n";
        ssize_t w = write(2, msg, sizeof msg - 1);
        (void)w;
        abort();
    }
    if (throw_frame == -2) throw_frame = throw_frame_of(rec);
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    char where[200];
    describe_address(rec->address, where, sizeof where);
    log("FATAL: %s: exception 0x%08x %s flags 0x%x at %s, thread %llu", why, rec->code, code_name(rec->code), rec->flags,
        where, static_cast<unsigned long long>(tid));
    for (uint32_t i = 0; i < rec->n_params && i < GTA6RT_X64_EXCEPTION_MAX_PARAMS; ++i)
        log("  param[%u] = 0x%llx", i, static_cast<unsigned long long>(rec->params[i]));
    if (rec->code == kStatusAccessViolation && rec->n_params >= 2) {
        const uint64_t k = rec->params[0];
        log("  %s at address 0x%llx", k == 0 ? "read" : k == 1 ? "write" : k == 8 ? "execute (DEP)" : "access",
            static_cast<unsigned long long>(rec->params[1]));
    }
    if (rec->code == kStatusStackBufferOverrun && rec->n_params >= 1)
        log("  fastfail code %llu", static_cast<unsigned long long>(rec->params[0]));
    describe_cxx(rec);
    if (ctx) {
        uint64_t pcs[64];
        const uint32_t n = backtrace(ctx, pcs, 64);
        log("  stack (%u frames, .pdata walk):", n);
        for (uint32_t i = 0; i < n; ++i) {
            describe_address(pcs[i], where, sizeof where);
            log("    #%-2u %s%s", i, where,
                static_cast<int>(i) != throw_frame ? ""
                : rec->code == kCxxExceptionCode ? "   <- throw site"
                                                 : "   <- raise site");
        }
        log("  rip %016llx rsp %016llx rbp %016llx eflags %08x", static_cast<unsigned long long>(ctx->rip),
            static_cast<unsigned long long>(ctx->rsp), static_cast<unsigned long long>(ctx->rbp), ctx->eflags);
        log("  rax %016llx rbx %016llx rcx %016llx rdx %016llx", static_cast<unsigned long long>(ctx->rax),
            static_cast<unsigned long long>(ctx->rbx), static_cast<unsigned long long>(ctx->rcx),
            static_cast<unsigned long long>(ctx->rdx));
        log("  rsi %016llx rdi %016llx r8  %016llx r9  %016llx", static_cast<unsigned long long>(ctx->rsi),
            static_cast<unsigned long long>(ctx->rdi), static_cast<unsigned long long>(ctx->r8),
            static_cast<unsigned long long>(ctx->r9));
        log("  r10 %016llx r11 %016llx r12 %016llx r13 %016llx", static_cast<unsigned long long>(ctx->r10),
            static_cast<unsigned long long>(ctx->r11), static_cast<unsigned long long>(ctx->r12),
            static_cast<unsigned long long>(ctx->r13));
        log("  r14 %016llx r15 %016llx", static_cast<unsigned long long>(ctx->r14), static_cast<unsigned long long>(ctx->r15));
    }
    abort();
}

bool dispatch_exception(rec_t* rec, ctx_t* ctx) {
    init_blob();
    active_guard guard;
    if (verbose()) {
        char where[200];
        describe_address(rec->address, where, sizeof where);
        log("dispatch 0x%08x %s flags 0x%x at %s", rec->code, code_name(rec->code), rec->flags, where);
    }
    if (rec->code == kStatusStackBufferOverrun) fatal_exception(rec, ctx, "fastfail (not dispatched, as on Windows)", 0);
    if (call_vectored(false, rec, ctx) == -1) {
        call_vectored(true, rec, ctx);
        return true;
    }
    if (policy() == GTA6RT_SEH_LOG_AND_ABORT) {
        if (informational(rec->code) && !(rec->flags & kNonContinuable)) {
            char where[200];
            describe_address(rec->address, where, sizeof where);
            log("informational exception 0x%08x %s at %s continued (LOG_AND_ABORT)", rec->code, code_name(rec->code), where);
            return true;
        }
        fatal_exception(rec, ctx, "SEH policy LOG_AND_ABORT", -2);
    }
#if !defined(__x86_64__)
    fatal_exception(rec, ctx, "frame-based dispatch is x86_64-only; arm64 build aborts", -2);
#endif
    const uint32_t st = call_stack_handlers(rec, ctx);
    if (st == kStatusSuccess) {
        call_vectored(true, rec, ctx);
        return true;
    }
    if (st == kStatusInvalidDisposition) fatal_exception(rec, ctx, "a handler returned an invalid disposition", -2);
    const uint64_t filter = g_top_filter.load(std::memory_order_acquire);
    if (filter && !t_state.in_filter) {
        exception_pointers ptrs{rec, ctx};
        ++t_state.in_filter;
        const int32_t r = call_filter_like(filter, &ptrs, ctx);
        --t_state.in_filter;
        if (verbose()) log("top-level filter 0x%llx returned %d", static_cast<unsigned long long>(filter), r);
        if (r == -1 && !(rec->flags & kNonContinuable)) return true;
    }
    fatal_exception(rec, ctx, (rec->flags & kStackInvalid) ? "unhandled exception (stack invalid)" : "unhandled exception", -2);
}

void unwind(uint64_t end_frame, uint64_t target_ip, rec_t* rec, uint64_t retval, ctx_t* context, void* history,
            const ctx_t* start) {
    init_blob();
    const stack_bounds sb = current_stack();
    alignas(16) ctx_t local_context;
    alignas(16) ctx_t new_context;
    rec_t local_rec;
    if (!context) context = &local_context;
    memcpy(context, start, sizeof *context);
    memcpy(&new_context, context, sizeof new_context);
    if (!rec) {
        memset(&local_rec, 0, sizeof local_rec);
        local_rec.code = kStatusUnwind;
        local_rec.address = context->rip;
        rec = &local_rec;
    }
    rec->flags |= kUnwinding | (end_frame ? 0 : kExitUnwind);
    if (verbose())
        log("unwind to frame 0x%llx ip 0x%llx code 0x%08x", static_cast<unsigned long long>(end_frame),
            static_cast<unsigned long long>(target_ip), rec->code);
    dispatcher_context d{};
    d.establisher_frame = context->rsp;
    d.target_ip = target_ip;
    d.context_record = context;
    d.history_table = history;
    bool skip_step = false;
    for (unsigned frames = 0;; ++frames) {
        if (frames > 100000) fatal_exception(rec, context, "unwind did not terminate", -1);
        if (!skip_step && !dispatch_step(kFlagUHandler, &d, &new_context))
            fatal_exception(rec, context, "unwind: unreadable unwind data", -1);
        skip_step = false;
        if (!d.establisher_frame) break;
        if (!valid_frame(d.establisher_frame, sb)) {
            rec->flags |= kStackInvalid;
            break;
        }
        if (d.language_handler) {
            if (end_frame && d.establisher_frame > end_frame)
                fatal_exception(rec, context, "STATUS_INVALID_UNWIND_TARGET: unwind passed its target frame", -1);
            if (d.establisher_frame == end_frame) rec->flags |= kTargetUnwind;
            const uint32_t r = call_unwind_handler(rec, &d);
            if (r == kDispCollided) {
                memcpy(&new_context, d.context_record, sizeof new_context);
                memcpy(context, &new_context, sizeof *context);
                d.context_record = context;
                unwind_out o;
                if (!virtual_unwind(0, d.image_base, d.control_pc, d.function_entry, &new_context, nullptr, false, &o))
                    fatal_exception(rec, context, "collided unwind: unreadable unwind data", -1);
                rec->flags |= kCollidedUnwind;
                skip_step = true;
                continue;
            }
            rec->flags &= ~kCollidedUnwind;
            if (r != kDispContinueSearch) fatal_exception(rec, context, "unwind handler returned an invalid disposition", -1);
        }
        if (d.establisher_frame == end_frame) break;
        memcpy(context, &new_context, sizeof *context);
    }
    if (!end_frame || d.establisher_frame != end_frame)
        fatal_exception(rec, context,
                        end_frame ? "unwind target frame not found on the guest stack" : "exit unwind reached the end of the stack", -1);
    if (rec->code != kStatusUnwindConsolidate) context->rip = target_ip;
    context->rax = retval;
    restore_context(context, rec);
}

void restore_context(ctx_t* c, rec_t* rec) {
    if (rec && rec->code == kStatusLongjump && rec->n_params >= 1) {
        const uint8_t* jb = reinterpret_cast<const uint8_t*>(rec->params[0]);
        uint64_t q[11];
        memcpy(q, jb, sizeof q);
        c->rbx = q[1];
        c->rsp = q[2];
        c->rbp = q[3];
        c->rsi = q[4];
        c->rdi = q[5];
        c->r12 = q[6];
        c->r13 = q[7];
        c->r14 = q[8];
        c->r15 = q[9];
        c->rip = q[10];
        uint32_t mxcsr;
        uint16_t fpcsr;
        memcpy(&mxcsr, jb + 0x58, 4);
        memcpy(&fpcsr, jb + 0x5c, 2);
        c->mxcsr = mxcsr;
        memcpy(c->fp.flt_save + 0, &fpcsr, 2);
        for (int i = 0; i < 10; ++i) memcpy(&c->fp.v.xmm[6 + i], jb + 0x60 + 16 * i, 16);
    } else if (rec && rec->code == kStatusUnwindConsolidate && rec->n_params >= 1) {
#if defined(__x86_64__)
        if (verbose())
            log("consolidate callback 0x%llx", static_cast<unsigned long long>(rec->params[0]));
        tramp_args a{};
        a.fn = rec->params[0];
        a.a[0] = reinterpret_cast<uint64_t>(rec);
        a.frame_ctx = c;
        c->rip = seh_tramp_consolidate(&a);
#endif
    }
    const uint32_t mx = c->mxcsr & 0xffffu;
    memcpy(c->fp.flt_save + 24, &mx, 4);
    seh_restore_raw(c);
}

uint64_t add_vectored(bool cont, bool first, uint64_t handler) {
    if (!handler) return 0;
    std::lock_guard<std::mutex> l(g_veh_mutex);
    if (g_veh_count >= 128) return 0;
    const veh_entry e{handler, ++g_cookie_next, cont};
    if (first) {
        memmove(&g_veh[1], &g_veh[0], sizeof(veh_entry) * static_cast<size_t>(g_veh_count));
        g_veh[0] = e;
    } else {
        g_veh[g_veh_count] = e;
    }
    ++g_veh_count;
    return e.cookie;
}

bool remove_vectored(bool cont, uint64_t cookie) {
    std::lock_guard<std::mutex> l(g_veh_mutex);
    for (int i = 0; i < g_veh_count; ++i)
        if (g_veh[i].cookie == cookie && g_veh[i].cont == cont) {
            memmove(&g_veh[i], &g_veh[i + 1], sizeof(veh_entry) * static_cast<size_t>(g_veh_count - i - 1));
            --g_veh_count;
            return true;
        }
    return false;
}

uint64_t set_top_filter(uint64_t filter) { return g_top_filter.exchange(filter); }

int32_t unhandled_exception_filter(rec_t* rec, ctx_t* ctx) {
    if (rec) {
        char where[200];
        describe_address(rec->address, where, sizeof where);
        log("UnhandledExceptionFilter: exception 0x%08x %s at %s", rec->code, code_name(rec->code), where);
        if (rec->code == kStatusStackBufferOverrun && rec->n_params >= 1)
            log("  fastfail/security failure code %llu", static_cast<unsigned long long>(rec->params[0]));
        describe_cxx(rec);
    }
    if (ctx) {
        uint64_t pcs[48];
        const uint32_t n = backtrace(ctx, pcs, 48);
        for (uint32_t i = 0; i < n; ++i) {
            char where[200];
            describe_address(pcs[i], where, sizeof where);
            log("    #%-2u %s", i, where);
        }
    }
    const uint64_t filter = g_top_filter.load(std::memory_order_acquire);
    if (filter && !t_state.in_filter && rec && ctx) {
        exception_pointers ptrs{rec, ctx};
        ++t_state.in_filter;
        const int32_t r = call_filter_like(filter, &ptrs, ctx);
        --t_state.in_filter;
        if (r == -1) return -1;
    }
    return 1;
}

void init_blob() {
#if defined(__x86_64__)
    static std::once_flag once;
    std::call_once(once, [] {
        table_add(reinterpret_cast<uint64_t>(seh_blob_base), seh_blob_pdata, seh_blob_pdata_count,
                  static_cast<uint64_t>(seh_blob_end - seh_blob_base), true);
    });
#endif
}

}  // namespace seh

extern "C" {

#if defined(__x86_64__)
__attribute__((ms_abi)) uint32_t seh_nested_handler_c(seh::rec_t* rec, uint64_t frame, seh::ctx_t*,
                                                      seh::dispatcher_context* d) {
    if (rec->flags & seh::kUnwindMask) return seh::kDispContinueSearch;
    seh::tramp_args* a;
    memcpy(&a, reinterpret_cast<const void*>(frame + 0x20), sizeof a);
    const auto* prev = static_cast<const seh::dispatcher_context*>(a->user);
    d->establisher_frame = prev->establisher_frame;
    return seh::kDispNested;
}

__attribute__((ms_abi)) uint32_t seh_collided_handler_c(seh::rec_t*, uint64_t frame, seh::ctx_t*,
                                                        seh::dispatcher_context* d) {
    seh::tramp_args* a;
    memcpy(&a, reinterpret_cast<const void*>(frame + 0x20), sizeof a);
    const auto* prev = static_cast<const seh::dispatcher_context*>(a->user);
    d->control_pc = prev->control_pc;
    d->image_base = prev->image_base;
    d->function_entry = prev->function_entry;
    d->establisher_frame = prev->establisher_frame;
    d->context_record = prev->context_record;
    d->language_handler = prev->language_handler;
    d->handler_data = prev->handler_data;
    d->history_table = prev->history_table;
    d->scope_index = prev->scope_index;
    return seh::kDispCollided;
}
#endif
}
