#pragma once

#include "isa_internal.h"

#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Debug/InternalThreadState.h>

#include <atomic>
#include <cstdint>

namespace gta6rt::fex {

struct FexThread {
    FEXCore::Core::InternalThreadState* thread = nullptr;
    FEXCore::Core::CpuStateFrame* frame = nullptr;
    gta6rt::isa::ThreadRec* rec = nullptr;
    uint8_t* callret = nullptr;  // allocation including both guard pages
    size_t callret_size = 0;
    uint8_t* own_stack = nullptr;  // separate guest stack (small host stacks only)
    size_t own_stack_size = 0;
    uint64_t split_guard = 0;  // guard page between host and guest halves of the pthread stack
    uint64_t guest_top = 0;    // initial guest rsp region top for depth-0 calls
    uint64_t guest_floor = 0;  // lowest guest stack address
    uint64_t host_fpcr = 0;
    uint32_t depth = 0;
    uint32_t thunk_depth = 0;     // thunk handlers active on this thread (all host->guest levels)
    uint32_t thunk_level[64] = {};  // host->guest level (depth) of each active thunk handler
    uint64_t level_guest_rsp[64] = {};
    bool pending = false;
    gta6rt_x64_exception_record pending_rec{};
};

struct NativeThunk {
    const void* fn;
    gta6rt_native_sig sig;
};

FEXCore::Context::Context* context();
FexThread* current();
// Re-marshals the guest's Win64 call (state in frame) into an AAPCS64 call of t->fn; false if the stack arguments do
// not fit.
bool native_call(const NativeThunk* t, FEXCore::Core::CpuStateFrame* frame);

}  // namespace gta6rt::fex
