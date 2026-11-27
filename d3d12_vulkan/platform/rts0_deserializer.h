// D3D12CreateRootSignatureDeserializer / D3D12CreateVersionedRootSignatureDeserializer without a host d3d12.dll: reads
// the RTS0 part of a DXBC container (or a bare RTS0 payload) into the D3D12 description structs. The FidelityFX
// upscaler DLL maps its root CBVs to root-parameter indices through it; without one it binds none of them.
#pragma once

#include <d3d12.h>

namespace rt::d3d12_vulkan {

HRESULT create_root_signature_deserializer_rts0(const void* data, SIZE_T size, REFIID iid, void** out);
HRESULT create_versioned_root_signature_deserializer_rts0(const void* data, SIZE_T size, REFIID iid, void** out);

}  // namespace rt::d3d12_vulkan
