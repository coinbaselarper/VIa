# Included by d3d12_metal.cmake: the host-served FidelityFX upscaler (STATUS.md in this folder).
target_sources(gta6rt_d3d12_metal_objs PRIVATE
  ${D3D12MTL_DIR}/upscale/src/ffx_exports.cpp
  ${D3D12MTL_DIR}/upscale/src/metal_upscaler.mm
  ${D3D12MTL_DIR}/upscale/src/metal_stage.mm)
target_include_directories(gta6rt_d3d12_metal_objs BEFORE PRIVATE "${D3D12MTL_DIR}/upscale/include")
set_source_files_properties(${D3D12MTL_DIR}/upscale/src/ffx_exports.cpp PROPERTIES
  COMPILE_OPTIONS "-include;${D3D12VK_DIR}/platform/win32_host.h"
  OBJECT_DEPENDS "${_overlay}")
set_source_files_properties(${D3D12MTL_DIR}/upscale/src/metal_upscaler.mm ${D3D12MTL_DIR}/upscale/src/metal_stage.mm
  PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-xobjective-c++;-fobjc-arc;-I${GTA6RT_ROOT}/metal/src")
cmake_language(DEFER CALL target_link_libraries gta6rt_d3d12_metal INTERFACE "-framework MetalFX")
