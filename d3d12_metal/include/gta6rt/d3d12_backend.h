#pragma once

/*
 * d3d12_xs.dll backend selection. gta6rt_d3d12_metal registers leonida's D3D12 front end on the Metal backend
 * (port/metal); gta6rt_d3d12_backend_register picks between it and gta6rt_d3d12_vulkan_register by GTA6RT_GPU:
 * unset or "vulkan" = Vulkan (default), "metal" = Metal. Any other value fails with GTA6RT_E_INVALID_ARG.
 */

#include <gta6rt/abi.h>

GTA6RT_API_BEGIN

gta6rt_status gta6rt_d3d12_metal_register(void);
/* Writes the pipeline archive and stops the shader builders, waiting up to wait_ms for running builds. */
gta6rt_status gta6rt_d3d12_metal_at_exit(uint32_t wait_ms);
gta6rt_status gta6rt_d3d12_backend_register(void);
/* The selected backend's exit hook (gta6rt_d3d12_vulkan_at_exit or gta6rt_d3d12_metal_at_exit). */
gta6rt_status gta6rt_d3d12_backend_at_exit(uint32_t wait_ms);
/* "vulkan" or "metal" after gta6rt_d3d12_backend_register, NULL before. */
const char* gta6rt_d3d12_backend_name(void);

GTA6RT_API_END
