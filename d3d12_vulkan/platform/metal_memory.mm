#import <Metal/Metal.h>

#include "metal_memory.h"

namespace gta6rt::metal_memory {

namespace {
id<MTLDevice> device() {
    static id<MTLDevice> d = MTLCreateSystemDefaultDevice();
    return d;
}
}  // namespace

Sizes query() {
    Sizes s;
    id<MTLDevice> d = device();
    if (!d) return s;
    s.valid = true;
    s.current_allocated = [d currentAllocatedSize];
    s.recommended_working_set = [d recommendedMaxWorkingSetSize];
    return s;
}

}  // namespace gta6rt::metal_memory
