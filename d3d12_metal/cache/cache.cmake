# Included by d3d12_metal.cmake: the pipeline cache's recorder and prebuild in the D3D12 Metal layer (STATUS-cache.md).
target_sources(gta6rt_d3d12_metal_objs PRIVATE
  ${D3D12MTL_DIR}/cache/src/codec.cpp
  ${D3D12MTL_DIR}/cache/src/pcache.cpp
  ${D3D12MTL_DIR}/cache/src/phase.cpp)
target_include_directories(gta6rt_d3d12_metal_objs BEFORE PRIVATE "${D3D12MTL_DIR}/cache/include")
