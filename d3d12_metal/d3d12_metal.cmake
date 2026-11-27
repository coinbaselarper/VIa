# D3D12 -> gta6rt::gpu (port/metal): leonida's D3D12 front end from port/d3d12_vulkan on the Metal backend, with no
# Vulkan driver. Added by superbuild/attach.cmake (or by the integrator) in a tree that configures d3d12_vulkan,
# whose sources it compiles by path and whose generated d3d12.h / d3d12_stubs.h it uses.
#
# Front-end sources that make no Vulkan call are compiled from port/d3d12_vulkan unchanged. The headers they include
# that are Vulkan-bound are replaced for this module only through a clang VFS overlay (d3d12_vulkan's own build is
# untouched). Files that mix Vulkan and neutral code are copies under shims/ (ORIGIN.md).
#
# The objects are prelinked (ld -r) into one object that exports only gta6rt_d3d12_metal_* and the D3D12 trace
# switch, so a host can force-load this library and gta6rt_d3d12_vulkan side by side: both define leonida's classes.


set(D3D12VK_DIR "${GTA6RT_ROOT}/d3d12_vulkan")
get_filename_component(D3D12MTL_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
if(NOT D3D12MTL_BIN)
  set(D3D12MTL_BIN "${D3D12MTL_BIN}")
endif()

set(_overlay "${D3D12MTL_BIN}/d3d12_metal_overlay.yaml")
set(_overlay_pairs
  "${D3D12VK_DIR}/shims/d3d12_com.h|${D3D12MTL_DIR}/shims/d3d12_com.h"
  "${D3D12VK_DIR}/shims/dxgi_com.h|${D3D12MTL_DIR}/shims/dxgi_com.h"
  "${D3D12VK_DIR}/shims/d3d12_pipeline_compile.h|${D3D12MTL_DIR}/src/metal_pipeline_compile.h"
  "${D3D12VK_DIR}/src/vulkan_present.h|${D3D12MTL_DIR}/src/metal_present_planes.h"
  "${D3D12VK_DIR}/src/vulkan_backend.h|${D3D12MTL_DIR}/src/metal_context.h")
set(_roots "")
foreach(_pair IN LISTS _overlay_pairs)
  string(REPLACE "|" ";" _p "${_pair}")
  list(GET _p 0 _from)
  list(GET _p 1 _to)
  if(NOT EXISTS "${_from}")
    message(FATAL_ERROR "d3d12_metal overlay: ${_from} is gone from d3d12_vulkan")
  endif()
  string(APPEND _roots "    { \"type\": \"file\", \"name\": \"${_from}\", \"external-contents\": \"${_to}\" },\n")
endforeach()
string(REGEX REPLACE ",\n$" "\n" _roots "${_roots}")
file(CONFIGURE OUTPUT "${_overlay}" CONTENT
  "{ \"version\": 0, \"case-sensitive\": \"false\", \"use-external-names\": true, \"roots\": [\n${_roots}  ] }\n")

set(D3D12MTL_UPSTREAM_SOURCES
  ${D3D12VK_DIR}/shims/d3d12_xs_device.cpp
  ${D3D12VK_DIR}/shims/d3d12_xs.cpp
  ${D3D12VK_DIR}/shims/xbox_present.cpp
  ${D3D12VK_DIR}/shims/package_splash.cpp)
set(D3D12MTL_UPSTREAM_PLATFORM
  ${D3D12VK_DIR}/platform/image_decode.cpp
  ${D3D12VK_DIR}/platform/display_info.cpp
  ${D3D12VK_DIR}/platform/win32_host.cpp
  ${D3D12VK_DIR}/platform/register.cpp
  ${D3D12VK_DIR}/platform/rts0_serializer.cpp
  ${D3D12VK_DIR}/platform/rts0_deserializer.cpp
  ${D3D12VK_DIR}/platform/title_memory_probe.cpp
  ${D3D12VK_DIR}/platform/fault_guard.cpp)
set(D3D12MTL_FORKED_SOURCES
  ${D3D12MTL_DIR}/shims/d3d12_com.cpp
  ${D3D12MTL_DIR}/shims/d3d12_metal_device.cpp
  ${D3D12MTL_DIR}/shims/dxgi_com.cpp)
set(D3D12MTL_OWN_SOURCES
  ${D3D12MTL_DIR}/src/metal_context.cpp
  ${D3D12MTL_DIR}/src/metal_replay.cpp
  ${D3D12MTL_DIR}/src/metal_present.cpp
  ${D3D12MTL_DIR}/src/metal_shaders.cpp
  ${D3D12MTL_DIR}/src/metal_bindless.cpp
  ${D3D12MTL_DIR}/platform/register_metal.cpp
  ${GTA6RT_ROOT}/shaderc_msl/src/catalog.cpp)

add_library(gta6rt_d3d12_metal_objs OBJECT
  ${D3D12MTL_UPSTREAM_SOURCES} ${D3D12MTL_UPSTREAM_PLATFORM} ${D3D12MTL_FORKED_SOURCES} ${D3D12MTL_OWN_SOURCES})
add_dependencies(gta6rt_d3d12_metal_objs gta6rt_d3d12_vulkan_generated)
target_include_directories(gta6rt_d3d12_metal_objs BEFORE PRIVATE
  "${D3D12MTL_DIR}/include" "${D3D12MTL_DIR}/shims" "${D3D12MTL_DIR}/src" "${GTA6RT_ROOT}/shaderc_msl/include")
include("${D3D12MTL_DIR}/cache/cache.cmake")
target_link_libraries(gta6rt_d3d12_metal_objs PRIVATE
  gta6rt_interfaces gta6rt_d3d12_vulkan_headers gta6rt_dxguids gta6rt_shims_core gta6rt_os gta6rt_metal)
if(TARGET gta6rt_isa)
  target_link_libraries(gta6rt_d3d12_metal_objs PRIVATE gta6rt_isa)
endif()
target_compile_definitions(gta6rt_d3d12_metal_objs PRIVATE GTA6RT_D3D12_METAL=1
  GTA6RT_D3D12_METAL_PORT_ROOT="${GTA6RT_ROOT}")
target_compile_options(gta6rt_d3d12_metal_objs PRIVATE
  "-ivfsoverlay" "${_overlay}"
  $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-declarations -Wno-unused-parameter>)
set_source_files_properties(${D3D12MTL_UPSTREAM_SOURCES} ${D3D12MTL_FORKED_SOURCES} PROPERTIES
  COMPILE_OPTIONS "-include;${D3D12VK_DIR}/platform/win32_host.h"
  OBJECT_DEPENDS "${_overlay}")
# -DGTA6RT_D3D12_METAL_ASAN=ON (a diagnostic tree): this module and port/metal under AddressSanitizer, recoverable.
option(GTA6RT_D3D12_METAL_ASAN "AddressSanitizer for the D3D12-on-Metal layer and the Metal backend" OFF)
if(GTA6RT_D3D12_METAL_ASAN)
  foreach(_t gta6rt_d3d12_metal_objs gta6rt_metal)
    if(TARGET ${_t})
      target_compile_options(${_t} PRIVATE -fsanitize=address -fsanitize-recover=address -fno-omit-frame-pointer)
    endif()
  endforeach()
endif()

# Besides the entry points: the process-wide state the front end shares with the host and the shims (title-memory
# notes, XG layouts, value sinks, labels, DirectStorage staging, traces). Their inline functions and statics are
# exported weak, so they coalesce with the host's and the shims' copies instead of forming a second, empty set.
set(_shared_namespaces
  "2rt11d3d12_trace" "2rt4xmem" "2rt5xg_xs" "2rt13startup_trace"
  "7leonida5label" "7leonida15title_gpu_views" "7leonida18title_texel_writes" "7leonida16title_xg_layouts"
  "7leonida10value_sink" "7leonida13kernel_writes" "7leonida8dstorage" "7leonida15console_texture")
set(_export_text "_gta6rt_d3d12_metal_*\n")
foreach(_ns IN LISTS _shared_namespaces)
  foreach(_prefix "__ZN" "__ZZN" "__ZGVZN" "__ZTHN" "__ZTWN")
    string(APPEND _export_text "${_prefix}${_ns}*\n")
  endforeach()
endforeach()
set(_exports "${D3D12MTL_BIN}/d3d12_metal_exports.txt")
file(CONFIGURE OUTPUT "${_exports}" CONTENT "${_export_text}")
set(_prelinked "${D3D12MTL_BIN}/gta6rt_d3d12_metal_prelinked.o")
add_custom_command(OUTPUT "${_prelinked}"
  COMMAND "${CMAKE_LINKER}" -r -arch "${GTA6RT_ARCH}" -exported_symbols_list "${_exports}"
          -o "${_prelinked}" "$<TARGET_OBJECTS:gta6rt_d3d12_metal_objs>"
  COMMAND "${D3D12MTL_DIR}/tools/check_prelink.sh" "${_prelinked}"
  DEPENDS gta6rt_d3d12_metal_objs $<TARGET_OBJECTS:gta6rt_d3d12_metal_objs> "${_exports}"
  COMMAND_EXPAND_LISTS
  COMMENT "Prelinking gta6rt_d3d12_metal (exports gta6rt_d3d12_metal_* only)"
  VERBATIM)
set_source_files_properties("${_prelinked}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_library(gta6rt_d3d12_metal STATIC "${_prelinked}")
set_target_properties(gta6rt_d3d12_metal PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(gta6rt_d3d12_metal INTERFACE "${D3D12MTL_DIR}/include")
target_link_libraries(gta6rt_d3d12_metal INTERFACE gta6rt_metal gta6rt_shims_core gta6rt_os gta6rt_dxguids
  gta6rt_d3d12_portable "-framework Metal" "-framework QuartzCore" "-framework ImageIO" "-framework CoreGraphics"
  "-framework CoreFoundation" "-framework Cocoa")

add_library(gta6rt_d3d12_select STATIC ${D3D12MTL_DIR}/platform/backend_select.cpp)
target_include_directories(gta6rt_d3d12_select PUBLIC "${D3D12MTL_DIR}/include")
target_link_libraries(gta6rt_d3d12_select PRIVATE gta6rt_interfaces gta6rt_build_options)
