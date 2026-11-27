// Measurement tool (built by hand, like metal_cache_probe.mm):
//   clang++ -std=c++20 -fobjc-arc -O2 -arch x86_64 tests/nocopy_unmap_probe.mm -framework Metal -framework Foundation
// What Metal does with a no-copy buffer (newBufferWithBytesNoCopy, what MoltenVK's VK_EXT_external_memory_host import
// makes) when the host pages under it change while it lives, and when it is released while a command buffer that
// uses it is in flight. Each case: one blit from the buffer, waited for, its status and error code printed.
#import <Metal/Metal.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace {

id<MTLDevice> g_device;
id<MTLCommandQueue> g_queue;

const char* status_name(MTLCommandBufferStatus s) {
    switch (s) {
    case MTLCommandBufferStatusCompleted: return "completed";
    case MTLCommandBufferStatusError: return "error";
    default: return "other";
    }
}

void report(const char* what, id<MTLCommandBuffer> cb, double ms) {
    std::printf("%-64s %s", what, status_name(cb.status));
    if (cb.error) std::printf(" (code %ld: %s)", long(cb.error.code), cb.error.localizedDescription.UTF8String);
    std::printf(" [%.2f ms]\n", ms);
}

id<MTLCommandBuffer> blit_from(id<MTLBuffer> source, id<MTLBuffer> target, NSUInteger bytes, bool unretained) {
    id<MTLCommandBuffer> cb = unretained ? [g_queue commandBufferWithUnretainedReferences] : [g_queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromBuffer:source sourceOffset:0 toBuffer:target destinationOffset:0 size:bytes];
    [blit endEncoding];
    return cb;
}

void* host_pages(std::size_t bytes) {
    mach_vm_address_t at = 0;
    if (mach_vm_allocate(mach_task_self(), &at, bytes, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) return nullptr;
    std::memset(reinterpret_cast<void*>(at), 0x5a, bytes);
    return reinterpret_cast<void*>(at);
}

}  // namespace

int main() {
    @autoreleasepool {
        g_device = MTLCreateSystemDefaultDevice();
        g_queue = [g_device newCommandQueue];
        const std::size_t bytes = 1u << 20;
        id<MTLBuffer> target = [g_device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        std::printf("device %s, page size %ld\n", g_device.name.UTF8String, long(getpagesize()));

        {
            void* pages = host_pages(bytes);
            id<MTLBuffer> b = [g_device newBufferWithBytesNoCopy:pages length:bytes options:MTLResourceStorageModeShared deallocator:nil];
            id<MTLCommandBuffer> cb = blit_from(b, target, bytes, true);
            const auto t = std::chrono::steady_clock::now();
            [cb commit];
            [cb waitUntilCompleted];
            report("1 baseline: no-copy buffer, pages intact", cb,
                   std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count());
            mach_vm_deallocate(mach_task_self(), mach_vm_address_t(pages), bytes);
        }
        {
            void* pages = host_pages(bytes);
            id<MTLBuffer> b = [g_device newBufferWithBytesNoCopy:pages length:bytes options:MTLResourceStorageModeShared deallocator:nil];
            const auto t0 = std::chrono::steady_clock::now();
            const kern_return_t kr = mach_vm_deallocate(mach_task_self(), mach_vm_address_t(pages), bytes);
            std::printf("2 mach_vm_deallocate under a live no-copy buffer: kr %d in %.2f ms\n", kr,
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            id<MTLCommandBuffer> cb = blit_from(b, target, bytes, true);
            const auto t = std::chrono::steady_clock::now();
            [cb commit];
            [cb waitUntilCompleted];
            report("2 then a blit from the buffer", cb,
                   std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count());
        }
        {
            void* pages = host_pages(bytes);
            id<MTLBuffer> b = [g_device newBufferWithBytesNoCopy:pages length:bytes options:MTLResourceStorageModeShared deallocator:nil];
            void* other = host_pages(bytes);
            mach_vm_address_t at = mach_vm_address_t(pages);
            vm_prot_t cur = 0, max = 0;
            const kern_return_t kr = mach_vm_remap(mach_task_self(), &at, bytes, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                                                   mach_task_self(), mach_vm_address_t(other), FALSE, &cur, &max,
                                                   VM_INHERIT_NONE);
            std::printf("3 mach_vm_remap over a live no-copy buffer's pages (xmem's alias): kr %d\n", kr);
            id<MTLCommandBuffer> cb = blit_from(b, target, bytes, true);
            const auto t = std::chrono::steady_clock::now();
            [cb commit];
            [cb waitUntilCompleted];
            report("3 then a blit from the buffer", cb,
                   std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count());
        }
        {
            void* pages = host_pages(bytes);
            id<MTLBuffer> b = [g_device newBufferWithBytesNoCopy:pages length:bytes options:MTLResourceStorageModeShared deallocator:nil];
            const int r = mprotect(pages, bytes, PROT_READ);
            id<MTLCommandBuffer> cb = blit_from(b, target, bytes, true);
            const auto t = std::chrono::steady_clock::now();
            [cb commit];
            [cb waitUntilCompleted];
            char what[96];
            std::snprintf(what, sizeof what, "4 pages made read-only (mprotect %d), then a blit", r);
            report(what, cb, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count());
            mprotect(pages, bytes, PROT_READ | PROT_WRITE);
        }
        {
            id<MTLBuffer> big = [g_device newBufferWithLength:64u << 20 options:MTLResourceStorageModeShared];
            id<MTLBuffer> big_target = [g_device newBufferWithLength:64u << 20 options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb = blit_from(big, big_target, 64u << 20, true);
            [cb commit];
            big = nil;
            const auto t = std::chrono::steady_clock::now();
            [cb waitUntilCompleted];
            report("5 buffer released after commit, unretained references", cb,
                   std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count());
        }
        {
            id<MTLBuffer> big = [g_device newBufferWithLength:64u << 20 options:MTLResourceStorageModeShared];
            id<MTLBuffer> big_target = [g_device newBufferWithLength:64u << 20 options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb = blit_from(big, big_target, 64u << 20, true);
            big = nil;
            [cb commit];
            const auto t = std::chrono::steady_clock::now();
            [cb waitUntilCompleted];
            report("6 buffer released after encoding, before commit, unretained", cb,
                   std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count());
        }
    }
    return 0;
}
