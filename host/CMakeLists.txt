# The gta6rt executable. Owned by the integrator. verify.sh s1 reads build/macos/gta6rt (x86_64 tree).
set(_gta6rt_host_sources src/main.cpp src/trace.cpp src/window_shim.cpp src/d3d12_trace_switch.cpp src/title_watch.cpp)
list(APPEND _gta6rt_host_sources src/governor/governor.cpp src/governor/title_hooks.cpp src/governor/gpu_metal.mm)
if(GTA6RT_ARCH STREQUAL "x86_64")
  list(APPEND _gta6rt_host_sources src/trace_x86_64.S)
endif()
add_executable(gta6rt ${_gta6rt_host_sources})
set_target_properties(gta6rt PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_compile_definitions(gta6rt PRIVATE GTA6RT_SOURCE_ROOT="${GTA6RT_ROOT}")
target_include_directories(gta6rt PRIVATE "${GTA6RT_ROOT}/d3d12_vulkan/platform")
set_source_files_properties(src/d3d12_trace_switch.cpp PROPERTIES INCLUDE_DIRECTORIES "${GTA6RT_ROOT}/portable/shims")
target_compile_options(gta6rt PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-unused-parameter>)
set_source_files_properties(src/governor/gpu_metal.mm PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-x;objective-c++;-fobjc-arc")
target_link_libraries(gta6rt PRIVATE "-framework Metal" "-framework IOKit")

set(_gta6rt_force_load gta6rt_shims_core gta6rt_shims_gdk gta6rt_d3d12_vulkan)
target_link_libraries(gta6rt PRIVATE gta6rt_build_options gta6rt_wincompat ${_gta6rt_force_load} gta6rt_media
  gta6rt_loader gta6rt_seh gta6rt_isa gta6rt_os gta6rt_portable)
target_link_options(gta6rt PRIVATE "LINKER:-no_warn_duplicate_libraries")
foreach(_lib IN LISTS _gta6rt_force_load)
  target_link_options(gta6rt PRIVATE "LINKER:-force_load,$<TARGET_FILE:${_lib}>")
endforeach()

# 8 MiB main-thread stack (the title's /STACK). x86_64: host image at 1 TiB like leonida's /BASE (-image_base needs
# -no_pie); gta6rt_os adds -pagezero_size 0x10000 so the title's 32-bit heap fits below 4 GiB (arm64 keeps the 4 GiB
# __PAGEZERO, the kernel refuses a smaller one). Unhinted mmaps still start near 0x108000000, so main.cpp also holds
# [0x140000000, 0x200000000) from a constructor until the exe and package DLLs are mapped.
target_link_options(gta6rt PRIVATE "LINKER:-stack_size,0x800000")
if(GTA6RT_ARCH STREQUAL "x86_64")
  target_link_options(gta6rt PRIVATE "LINKER:-no_pie" "LINKER:-image_base,0x10000000000")
endif()

add_library(gta6rt_vkguard MODULE src/vk_guard.cpp)
set_target_properties(gta6rt_vkguard PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" PREFIX "lib" SUFFIX ".dylib"
  CXX_VISIBILITY_PRESET hidden)
target_link_libraries(gta6rt_vkguard PRIVATE gta6rt_vulkan_headers)
add_dependencies(gta6rt gta6rt_vkguard)

include(src/win/win.cmake)

add_subdirectory("${GTA6RT_ROOT}/hud" "${CMAKE_BINARY_DIR}/hud")
target_link_libraries(gta6rt PRIVATE gta6rt_hud)
