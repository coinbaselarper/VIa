#pragma once
// Fault-safe reads of the title's memory from host code, and the D3D12 layer's vectored exception handlers.
//
// A page the title unmaps between a readability check and the read must not end the process. The copy and compare
// loops below are small assembly routines whose fault addresses are known: a chained SIGSEGV/SIGBUS handler, installed
// in front of the isa/seh handlers, resumes a faulting routine at its failure exit (the Linux kernel's exception-table
// idea). Faults anywhere else go on down the chain unchanged.
//
// The same handler can dispatch write faults to the layer's AddVectoredExceptionHandler handlers (EXCEPTION_POINTERS
// in the Windows layout). Off unless LEONIDA_HOST_VEH=1: the layer's write guards (ShadowPages, the geometry watch)
// then switch on, and they need xmem to report aliases and the file shims to pin kernel writes (title_page_watch.h),
// which neither does on macOS yet.

#include <cstddef>
#include <cstdint>

namespace rthost::fault_guard {

// Installs the chained handler once. False when sigaction refused.
bool install();

// memcpy that returns false instead of faulting; `to` may be partly written then.
bool copy(void* to, const void* from, std::size_t bytes);

// 0: equal, 1: they differ, -1: a read of either faulted.
int compare(const void* a, const void* b, std::size_t bytes);

// `count` 64-byte samples from `from`, `stride` bytes apart, packed into `to`; false when a read faulted.
bool gather(void* to, const void* from, std::size_t count, std::size_t stride);

// Recovered faults so far (copies and compares that returned a fault).
std::uint64_t recovered();

using VectoredHandler = long (*)(void* exception_pointers);
// False when LEONIDA_HOST_VEH is not 1 or the table is full.
bool add_vectored_handler(bool first, VectoredHandler handler);
bool remove_vectored_handler(VectoredHandler handler);

}  // namespace rthost::fault_guard
