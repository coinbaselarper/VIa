#pragma once

#include <stddef.h>
#include <stdint.h>

#include <gta6rt/seh.h>
#include <gta6rt/x64_state.h>

namespace seh {

using ctx_t = gta6rt_x64_context;
using rec_t = gta6rt_x64_exception_record;
using rf_t = gta6rt_runtime_function;

constexpr uint32_t kFlagEHandler = 1, kFlagUHandler = 2, kFlagChainInfo = 4;

constexpr uint32_t kNonContinuable = 0x1, kUnwinding = 0x2, kExitUnwind = 0x4, kStackInvalid = 0x8,
                   kNestedCall = 0x10, kTargetUnwind = 0x20, kCollidedUnwind = 0x40;
constexpr uint32_t kUnwindMask = kUnwinding | kExitUnwind | kTargetUnwind | kCollidedUnwind;

constexpr uint32_t kDispContinueExecution = 0, kDispContinueSearch = 1, kDispNested = 2, kDispCollided = 3;

constexpr uint32_t kStatusSuccess = 0;
constexpr uint32_t kStatusBreakpoint = 0x80000003u, kStatusSingleStep = 0x80000004u;
constexpr uint32_t kStatusLongjump = 0x80000026u, kStatusUnwindConsolidate = 0x80000029u;
constexpr uint32_t kStatusAccessViolation = 0xC0000005u, kStatusInvalidDisposition = 0xC0000026u;
constexpr uint32_t kStatusNonContinuable = 0xC0000025u, kStatusUnwind = 0xC0000027u;
constexpr uint32_t kStatusBadStack = 0xC0000028u, kStatusInvalidUnwindTarget = 0xC0000029u;
constexpr uint32_t kStatusIllegalInstruction = 0xC000001Du, kStatusPrivInstruction = 0xC0000096u;
constexpr uint32_t kStatusStackOverflow = 0xC00000FDu, kStatusStackBufferOverrun = 0xC0000409u;
constexpr uint32_t kStatusAssertionFailure = 0xC0000420u, kStatusUnhandled = 0xC0000144u;
constexpr uint32_t kCxxExceptionCode = 0xE06D7363u;

constexpr uint64_t kContextAmd64 = 0x00100000u;
constexpr uint32_t kContextFull = 0x0010000Fu;

struct dispatcher_context {
    uint64_t control_pc;
    uint64_t image_base;
    const rf_t* function_entry;
    uint64_t establisher_frame;
    uint64_t target_ip;
    ctx_t* context_record;
    uint64_t language_handler;
    void* handler_data;
    void* history_table;
    uint32_t scope_index;
    uint32_t fill0;
};
static_assert(sizeof(dispatcher_context) == 80, "DISPATCHER_CONTEXT layout");

struct nonvol_ptrs {
    void* fp[16];
    uint64_t* gp[16];
};
static_assert(sizeof(nonvol_ptrs) == 256, "KNONVOLATILE_CONTEXT_POINTERS layout");

inline uint64_t* gpr(ctx_t* c, unsigned r) { return &c->rax + r; }
inline uint64_t gpr(const ctx_t* c, unsigned r) { return (&c->rax)[r]; }

// ---- memory -------------------------------------------------------------
bool safe_read(uint64_t addr, void* out, size_t n);

// ---- tables.cpp ----------------------------------------------------------
const rf_t* lookup(uint64_t pc, uint64_t* out_base);
bool pc_in_guest_image(uint64_t pc);
uint64_t image_base_of(uint64_t addr);
bool table_add(uint64_t base, const rf_t* table, uint32_t count, uint64_t image_size, bool is_image);
bool table_add_callback(uint64_t identifier, uint64_t base, uint32_t length, uint64_t callback, uint64_t context);
bool table_remove(const void* table_or_identifier);

// ---- unwind.cpp -----------------------------------------------------------
struct unwind_out {
    uint64_t handler;
    void* handler_data;
    uint64_t establisher;
    bool in_prolog;
    bool in_epilog;
};
// RtlVirtualUnwind over one frame. safe=true validates every memory read (backtraces of arbitrary contexts).
bool virtual_unwind(uint32_t type, uint64_t base, uint64_t pc, const rf_t* fn, ctx_t* c, nonvol_ptrs* ptrs, bool safe,
                    unwind_out* out);
// One step of a stack walk: function entry or leaf rule; false at the end of the guest stack or on bad data.
bool step_frame(ctx_t* c, bool safe, uint64_t* out_establisher);
uint32_t backtrace(const ctx_t* ctx, uint64_t* pcs, uint32_t max);

// ---- dispatch.cpp ---------------------------------------------------------
gta6rt_seh_policy policy();
void set_policy(gta6rt_seh_policy p);
void set_cxx_fallback(bool on);
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
bool verbose();
// Phase 1 + vectored handlers + policy. Returns true to continue execution with *ctx; never returns false
// (unhandled exceptions are logged and abort the process).
bool dispatch_exception(rec_t* rec, ctx_t* ctx);
[[noreturn]] void fatal_exception(const rec_t* rec, const ctx_t* ctx, const char* why, int throw_frame);
[[noreturn]] void fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// RtlUnwindEx with the caller's context already captured in *start.
[[noreturn]] void unwind(uint64_t target_frame, uint64_t target_ip, rec_t* rec, uint64_t retval, ctx_t* context,
                         void* history, const ctx_t* start);
[[noreturn]] void restore_context(ctx_t* c, rec_t* rec);

uint64_t add_vectored(bool continue_list, bool first, uint64_t handler);
bool remove_vectored(bool continue_list, uint64_t cookie);
uint64_t set_top_filter(uint64_t filter);
int32_t unhandled_exception_filter(rec_t* rec, ctx_t* ctx);
void describe_address(uint64_t pc, char* buf, size_t size);
void init_blob();

// ---- fault.cpp -------------------------------------------------------------
void register_isa_fault_handler();

// ---- arch (seh_x86_64.S on x86_64, arch_stub.cpp elsewhere) ---------------
struct tramp_args {
    uint64_t fn;
    uint64_t a[4];
    const ctx_t* frame_ctx;
    void* user;
};

}  // namespace seh

extern "C" {
uint64_t seh_tramp_plain(seh::tramp_args* a);
uint64_t seh_tramp_exception(seh::tramp_args* a);
uint64_t seh_tramp_unwind(seh::tramp_args* a);
uint64_t seh_tramp_consolidate(seh::tramp_args* a);
[[noreturn]] void seh_restore_raw(const seh::ctx_t* c);
[[noreturn]] void seh_restore_raw_ymm(const seh::ctx_t* c, const uint8_t* ymmh);
void seh_fault_entry();
extern const uint8_t seh_blob_base[];
extern const uint8_t seh_blob_end[];
extern const seh::rf_t seh_blob_pdata[];
extern const uint32_t seh_blob_pdata_count;
}
