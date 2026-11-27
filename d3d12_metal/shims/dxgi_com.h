#pragma once

// port/d3d12_metal: copy of d3d12_vulkan/shims/dxgi_com.h over the Metal backend (ORIGIN.md). IDXGIFactory7,
// IDXGIAdapter4 and IDXGIOutput6; swap chains are refused (the title presents through PresentX).

#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>

#include "../src/metal_context.h"

namespace rt::dxgi {

HRESULT create_factory(leonida::MetalContext& context, REFIID riid, void** out);

}  // namespace rt::dxgi
