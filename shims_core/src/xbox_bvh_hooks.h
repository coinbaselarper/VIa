// What the XboxBvhBuilder shim needs from the D3D12 layer (S1.6), which owns the proxies in front of the title's device
// and command lists and the lifetime of a list's resources. d3d12_vulkan registers these once; null hooks mean the
// pointers the title passes are already the layer's own objects and nothing holds scratch for a list.
#pragma once

#include <cstddef>

struct IUnknown;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;

namespace rt::xbox_bvh {

struct Hooks {
    ID3D12Device* (*device_proxy_real_device)(const void* device);
    IUnknown* (*real_object_behind_proxy)(const void* object);
    // Keep `object` alive until `list` is reset or released; false when the list cannot hold it.
    bool (*hold_until_reset)(ID3D12GraphicsCommandList* list, IUnknown* object);
};

void set_hooks(const Hooks& hooks);

// Builders constructed and not destroyed (tests).
size_t live_count();

}  // namespace rt::xbox_bvh
