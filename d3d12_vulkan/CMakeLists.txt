# S1.6 D3D12 -> Vulkan (KosmicKrisp / MoltenVK, loaded by dlopen) backend, DXGI, PresentX, CAMetalLayer window.
# Sources under shims/ and src/ are ported copies of leonida (ORIGIN.md); platform/ is new macOS code.

enable_language(OBJCXX)
set(D3D12VK_GEN "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(DXH_DIRECTX "${GTA6RT_THIRD_PARTY}/DirectX-Headers/include/directx")

add_custom_command(
  OUTPUT "${D3D12VK_GEN}/d3d12.h" "${D3D12VK_GEN}/stubsrc/d3d12.h"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${D3D12VK_GEN}/stubsrc"
  COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1 "${Python3_EXECUTABLE}" -I -B
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_d3d12_retval.py"
          "${DXH_DIRECTX}/d3d12.h" "${D3D12VK_GEN}/d3d12.h" "${D3D12VK_GEN}/stubsrc/d3d12.h"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_d3d12_retval.py" "${DXH_DIRECTX}/d3d12.h"
  VERBATIM)

add_custom_command(
  OUTPUT "${D3D12VK_GEN}/d3d12_stubs.h"
  COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1 "${Python3_EXECUTABLE}" -I -B
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_d3d12_stubs.py" --out "${D3D12VK_GEN}/d3d12_stubs.h"
          --header "${D3D12VK_GEN}/stubsrc/d3d12.h"
          ID3D12GraphicsCommandList8 ID3D12CommandQueue ID3D12CommandAllocator ID3D12Fence1
          ID3D12Device11 ID3D12DescriptorHeap ID3D12Heap ID3D12Resource ID3D12QueryHeap ID3D12RootSignature
          ID3D12PipelineState ID3D12CommandSignature ID3D12PipelineLibrary1 ID3D12ShaderCacheSession
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_d3d12_stubs.py" "${CMAKE_CURRENT_SOURCE_DIR}/tools/report_paths.py"
          "${D3D12VK_GEN}/stubsrc/d3d12.h"
  VERBATIM)
add_custom_target(gta6rt_d3d12_vulkan_generated DEPENDS "${D3D12VK_GEN}/d3d12.h" "${D3D12VK_GEN}/d3d12_stubs.h")

add_library(gta6rt_d3d12_vulkan_headers INTERFACE)
target_include_directories(gta6rt_d3d12_vulkan_headers BEFORE INTERFACE
  "${D3D12VK_GEN}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(gta6rt_d3d12_vulkan_headers INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/shims" "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/platform"
  "${CMAKE_CURRENT_SOURCE_DIR}/../hud/include")
target_compile_definitions(gta6rt_d3d12_vulkan_headers INTERFACE LEONIDA_VULKAN_DEVICE VK_USE_PLATFORM_METAL_EXT)
target_link_libraries(gta6rt_d3d12_vulkan_headers INTERFACE
  gta6rt_d3d12_portable gta6rt_vulkan_headers gta6rt_dxheaders gta6rt_build_options)

set(D3D12VK_LEONIDA_SOURCES
  shims/d3d12_com.cpp
  shims/d3d12_vulkan_device.cpp
  shims/d3d12_xs.cpp
  shims/d3d12_xs_device.cpp
  shims/dxgi_com.cpp
  shims/xbox_present.cpp
  shims/package_splash.cpp)
set(D3D12VK_PLATFORM_SOURCES
  platform/vk_driver.cpp
  platform/image_decode.cpp
  platform/display_info.cpp
  platform/metal_window.mm
  platform/metal_memory.mm
  platform/win32_host.cpp
  platform/register.cpp
  platform/xmem_fallback.cpp
  platform/rts0_serializer.cpp
  platform/rts0_deserializer.cpp
  platform/title_memory_probe.cpp
  platform/title_pages.cpp
  platform/fault_guard.cpp)

add_library(gta6rt_d3d12_vulkan STATIC ${D3D12VK_LEONIDA_SOURCES} ${D3D12VK_PLATFORM_SOURCES}
  "${D3D12VK_GEN}/d3d12_stubs.h")
add_dependencies(gta6rt_d3d12_vulkan gta6rt_d3d12_vulkan_generated)
target_link_libraries(gta6rt_d3d12_vulkan
  PUBLIC gta6rt_interfaces
  PRIVATE gta6rt_d3d12_vulkan_headers gta6rt_dxguids gta6rt_shims_core gta6rt_os)
if(TARGET gta6rt_isa)
  target_link_libraries(gta6rt_d3d12_vulkan PRIVATE gta6rt_isa)
endif()
target_link_libraries(gta6rt_d3d12_vulkan PUBLIC
  "-framework Cocoa" "-framework QuartzCore" "-framework Metal" "-framework ImageIO" "-framework CoreGraphics"
  "-framework CoreFoundation")
set_source_files_properties(${D3D12VK_LEONIDA_SOURCES} PROPERTIES
  COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/platform/win32_host.h")
set_source_files_properties(platform/metal_window.mm platform/metal_memory.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_compile_options(gta6rt_d3d12_vulkan PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-declarations -Wno-unused-parameter>)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
  add_subdirectory(tests)
endif()
