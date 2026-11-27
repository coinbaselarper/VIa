#pragma once

// The title's upscaler (FidelityFX through amd_fidelityfx_loader_xbox.dll, FSR4 on the Series S profile) served from
// the host. GTA6RT_UPSCALER=fsr4|metalfx-temporal|metalfx-spatial|bilinear (STATUS.md for the default).
//
// Backend-neutral half (ffx_exports.cpp): the five loader exports, the contexts, the jitter queries, and the dispatch,
// which records a marker `gta6rt-upscale#<frame>` on the title's command list and keeps the frame's description.
// Metal half (metal_stage.mm): the replay turns the marker into a split of the backend's command stream around the
// upscale, run on the device's own queue between the two halves (STATUS.md, "Stage in the command stream").

#include <cstdint>
#include <functional>
#include <string_view>

namespace gta6rt::gpu {
class CommandList;
class Device;
class Texture;
}  // namespace gta6rt::gpu

namespace gta6rt::upscale {

// Once per process: remembers the device and, unless the selection is fsr4, points the loader DLL's exports at the
// host. Call before the title can create an upscaler context (the D3D12 device's creation).
void install(gpu::Device& device);

bool is_marker(std::string_view label);

using TextureOf = std::function<gpu::Texture*(std::uintptr_t resource_id)>;
// Replay of a marker into `out` (Metal backend). False when the frame or its textures are unknown (nothing recorded).
bool record_metal(std::string_view label, gpu::CommandList& out, const TextureOf& texture);

}  // namespace gta6rt::upscale
