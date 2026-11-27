#import <Metal/Metal.h>
#include <IOKit/IOKitLib.h>

#include "gpu_metal.h"

namespace host::governor::gpu {

namespace {

id<MTLDevice> g_device = nil;

uint64_t number(CFDictionaryRef d, CFStringRef key, bool* found) {
    const void* v = CFDictionaryGetValue(d, key);
    if (!v || CFGetTypeID(v) != CFNumberGetTypeID()) return 0;
    int64_t n = 0;
    CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberSInt64Type, &n);
    *found = true;
    return n > 0 ? static_cast<uint64_t>(n) : 0;
}

}  // namespace

bool init() {
    if (g_device) return true;
    @autoreleasepool {
        g_device = MTLCreateSystemDefaultDevice();
    }
    return g_device != nil;
}

Info sample(bool with_system) {
    Info out;
    if (g_device) {
        out.have_device = true;
        out.allocated = g_device.currentAllocatedSize;
        out.working_set_limit = g_device.recommendedMaxWorkingSetSize;
        out.max_buffer = g_device.maxBufferLength;
        out.unified = g_device.hasUnifiedMemory;
    }
    if (!with_system) return out;
    io_iterator_t it = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAccelerator"), &it) != KERN_SUCCESS) return out;
    for (io_object_t e = IOIteratorNext(it); e != IO_OBJECT_NULL; e = IOIteratorNext(it)) {
        CFTypeRef p = IORegistryEntryCreateCFProperty(e, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0);
        if (p && CFGetTypeID(p) == CFDictionaryGetTypeID()) {
            const auto d = static_cast<CFDictionaryRef>(p);
            bool found = false;
            out.system_alloc += number(d, CFSTR("Alloc system memory"), &found);
            out.system_in_use += number(d, CFSTR("In use system memory"), &found);
            out.system_driver += number(d, CFSTR("In use system memory (driver)"), &found);
            out.recoveries += number(d, CFSTR("recoveryCount"), &found);
            out.have_system = out.have_system || found;
        }
        if (p) CFRelease(p);
        IOObjectRelease(e);
    }
    IOObjectRelease(it);
    return out;
}

}  // namespace host::governor::gpu
