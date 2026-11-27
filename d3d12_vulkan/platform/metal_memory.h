#pragma once

#include <cstdint>

namespace gta6rt::metal_memory {

// The system default MTLDevice's figures for this process (the device MoltenVK and KosmicKrisp drive on a Mac with
// one GPU). currentAllocatedSize counts every MTLResource this process holds on the device, bytesNoCopy buffers over
// host pages included.
struct Sizes {
    bool valid = false;
    std::uint64_t current_allocated = 0;
    std::uint64_t recommended_working_set = 0;
};
Sizes query();

}  // namespace gta6rt::metal_memory
