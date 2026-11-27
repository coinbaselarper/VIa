// D3D12SerializeVersionedRootSignature / D3D12SerializeRootSignature without a host d3d12.dll: writes the DXBC
// container with one RTS0 part in the layout rt::rootsig::parse_rts0 reads (and the PC serializer produces), Xbox
// high-byte flags cleared as on the forwarding path. The produced blob is what the device's CreateRootSignature takes.
#pragma once

#include <d3d12.h>

namespace rt::d3d12_vulkan {

HRESULT serialize_root_signature_rts0(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc, ID3DBlob** blob, ID3DBlob** error);
HRESULT serialize_root_signature_rts0_legacy(const D3D12_ROOT_SIGNATURE_DESC* desc, D3D_ROOT_SIGNATURE_VERSION version,
                                             ID3DBlob** blob, ID3DBlob** error);

}  // namespace rt::d3d12_vulkan
