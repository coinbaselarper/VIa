#include "gdk_title_memory.h"

#include <mach/mach.h>
#include <mach/mach_vm.h>

#include <atomic>

namespace rt::gdk {

namespace {
std::atomic<uintptr_t (*)(uintptr_t, uintptr_t)> g_to_cpu{nullptr};
std::atomic<uintptr_t (*)(uintptr_t)> g_other_view{nullptr};
std::atomic<void (*)(const void*, size_t)> g_texel_write{nullptr};
std::atomic<bool (*)(void*, const void*, size_t)> g_write{nullptr};
std::atomic<bool (*)(const void*, size_t)> g_commit{nullptr};
}  // namespace

void set_title_memory_hooks(const TitleMemoryHooks& hooks) {
    g_to_cpu = hooks.to_cpu;
    g_other_view = hooks.other_view;
    g_texel_write = hooks.texel_write;
    g_write = hooks.write;
    g_commit = hooks.commit;
}

bool title_write_hook_installed() { return g_write.load() != nullptr; }

uintptr_t title_to_cpu(uintptr_t address, uintptr_t bytes) {
    const auto fn = g_to_cpu.load();
    return fn ? fn(address, bytes) : address;
}

uintptr_t title_other_view(uintptr_t address) {
    const auto fn = g_other_view.load();
    return fn ? fn(address) : 0;
}

void title_texel_write(const void* begin, size_t bytes) {
    if (const auto fn = g_texel_write.load()) fn(begin, bytes);
}

bool title_write(void* destination, const void* source, size_t bytes) {
    if (const auto fn = g_write.load()) return fn(destination, source, bytes);
    return guarded_copy(destination, source, bytes);
}

bool title_commit(const void* address, size_t bytes) {
    const auto fn = g_commit.load();
    return fn ? fn(address, bytes) : true;
}

// The kernel copies up to 32 KiB by value. A larger copy moves VM objects instead: the source and destination ranges
// become new copy-on-write objects cut at 4 KiB, and under Rosetta (4 KiB task pages on a 16 KiB kernel) Metal then
// refuses a no-copy buffer (the D3D12 layer's host-memory import) over any 16 KiB page where two objects meet.
constexpr size_t kKernelCopyByValue = 32 * 1024;

bool guarded_copy(void* destination, const void* source, size_t bytes) {
    for (size_t done = 0; done < bytes;) {
        const size_t piece = bytes - done < kKernelCopyByValue ? bytes - done : kKernelCopyByValue;
        mach_vm_size_t copied = 0;
        const kern_return_t kr = mach_vm_read_overwrite(
            mach_task_self(), reinterpret_cast<mach_vm_address_t>(static_cast<const char*>(source) + done), piece,
            reinterpret_cast<mach_vm_address_t>(static_cast<char*>(destination) + done), &copied);
        if (kr != KERN_SUCCESS || copied != piece) return false;
        done += piece;
    }
    return true;
}

}  // namespace rt::gdk
