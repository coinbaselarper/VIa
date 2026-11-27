#pragma once

#include <cstddef>
#include <cstdint>

namespace rt::gdk {

// Accesses to the title's memory that the D3D12 layer wants to see or control. d3d12_vulkan registers these (the
// leonida title_gpu_views, title_texel_writes and kernel_writes of src/title_page_watch.h); without them the title's
// memory is plain memory.
struct TitleMemoryHooks {
    // Where the host reads a GPU-only alias range (title_gpu_views::to_cpu). Default: the address itself.
    uintptr_t (*to_cpu)(uintptr_t address, uintptr_t bytes) = nullptr;
    // The other view of a paired range, 0 outside one (diagnostics only).
    uintptr_t (*other_view)(uintptr_t address) = nullptr;
    // A write filled [begin, begin + bytes) (title_texel_writes::note).
    void (*texel_write)(const void* begin, size_t bytes) = nullptr;
    // A copy into title memory with its pages opened first (kernel_writes::write). Default: guarded_copy.
    bool (*write)(void* destination, const void* source, size_t bytes) = nullptr;
    // Commits pages of [address, address + bytes) that only a first touch would commit (xmem's first-touch pages), so
    // that a kernel-side copy does not fail on them. Default: nothing to commit (true).
    bool (*commit)(const void* address, size_t bytes) = nullptr;
};

void set_title_memory_hooks(const TitleMemoryHooks& hooks);
bool title_write_hook_installed();
uintptr_t title_to_cpu(uintptr_t address, uintptr_t bytes = 1);
uintptr_t title_other_view(uintptr_t address);
void title_texel_write(const void* begin, size_t bytes);
bool title_write(void* destination, const void* source, size_t bytes);
bool title_commit(const void* address, size_t bytes);

// memmove that reports an unreadable source or unwritable destination as false instead of faulting. The copy is made
// by the kernel (mach_vm_read_overwrite), so no signal handler is involved: pages that only a user-mode fault would
// commit (xmem's first-touch pages) must be committed first (rt::xmem::commit_physical_range).
bool guarded_copy(void* destination, const void* source, size_t bytes);

}  // namespace rt::gdk
