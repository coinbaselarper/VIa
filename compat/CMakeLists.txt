set(DXH "${GTA6RT_THIRD_PARTY}/DirectX-Headers/include")
set(VKH "${GTA6RT_THIRD_PARTY}/Vulkan-Headers/include")

add_library(gta6rt_wincompat INTERFACE)
target_include_directories(gta6rt_wincompat SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(gta6rt_wincompat INTERFACE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(gta6rt_wincompat INTERFACE gta6rt_build_options)

# compat/include must come before DirectX-Headers so <d3d12.h>, <windows.h>, <rpc.h>, <unknwn.h> resolve to the
# compat versions (the wrappers #include_next the real d3d12.h). DirectX-Headers' wsl/stubs are not used.
add_library(gta6rt_dxheaders INTERFACE)
target_include_directories(gta6rt_dxheaders SYSTEM INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include" "${DXH}" "${DXH}/directx" "${DXH}/dxguids")
target_link_libraries(gta6rt_dxheaders INTERFACE gta6rt_wincompat)

add_library(gta6rt_dxguids STATIC "${GTA6RT_THIRD_PARTY}/DirectX-Headers/src/dxguids.cpp")
target_link_libraries(gta6rt_dxguids PUBLIC gta6rt_dxheaders)

add_library(gta6rt_vulkan_headers INTERFACE)
target_include_directories(gta6rt_vulkan_headers SYSTEM INTERFACE "${VKH}")

add_executable(compat_abi_test tests/abi_test.cpp tests/abi_test_c.c)
target_link_libraries(compat_abi_test PRIVATE gta6rt_dxguids)
