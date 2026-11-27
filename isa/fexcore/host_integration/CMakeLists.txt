# gta6rt_fexhost: host/src built with the arm64 changes host/ needs (main.cpp.patch, trace.cpp.patch), so the
# fexcore backend can be run against the real title before the integrator merges them. Same link setup as
# host/CMakeLists.txt. The patches are applied at build time and fail the build if host/src has moved on.
set(_hsrc "${GTA6RT_ROOT}/host/src")
set(_hout "${CMAKE_CURRENT_BINARY_DIR}/src")
foreach(_f main trace)
  add_custom_command(OUTPUT "${_hout}/${_f}.cpp"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_hout}"
    COMMAND patch -s -o "${_hout}/${_f}.cpp" "${_hsrc}/${_f}.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/${_f}.cpp.patch"
    DEPENDS "${_hsrc}/${_f}.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/${_f}.cpp.patch" VERBATIM)
endforeach()

add_executable(gta6rt_fexhost "${_hout}/main.cpp" "${_hout}/trace.cpp" "${_hsrc}/window_shim.cpp"
  "${_hsrc}/d3d12_trace_switch.cpp")
set_target_properties(gta6rt_fexhost PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_compile_definitions(gta6rt_fexhost PRIVATE GTA6RT_SOURCE_ROOT="${GTA6RT_ROOT}")
target_include_directories(gta6rt_fexhost PRIVATE "${_hsrc}" "${GTA6RT_ROOT}/d3d12_vulkan/platform")
set_source_files_properties("${_hsrc}/d3d12_trace_switch.cpp" PROPERTIES INCLUDE_DIRECTORIES "${GTA6RT_ROOT}/portable/shims")
target_compile_options(gta6rt_fexhost PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-unused-parameter>)
set(_fl gta6rt_shims_core gta6rt_shims_gdk gta6rt_d3d12_vulkan)
target_link_libraries(gta6rt_fexhost PRIVATE gta6rt_build_options gta6rt_wincompat ${_fl}
  gta6rt_loader gta6rt_seh gta6rt_isa gta6rt_os gta6rt_portable)
target_link_options(gta6rt_fexhost PRIVATE "LINKER:-no_warn_duplicate_libraries" "LINKER:-stack_size,0x800000")
foreach(_lib IN LISTS _fl)
  target_link_options(gta6rt_fexhost PRIVATE "LINKER:-force_load,$<TARGET_FILE:${_lib}>")
endforeach()

# The window layer (host/src/win/win.cmake) is attached to gta6rt only; mirror it here while host/ is being split.
if(EXISTS "${_hsrc}/win/win_core.cpp")
  target_sources(gta6rt_fexhost PRIVATE "${_hsrc}/win/win_core.cpp" "${_hsrc}/win/appkit_window.mm")
  set_source_files_properties("${_hsrc}/win/appkit_window.mm" PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-x;objective-c++;-fobjc-arc")
  target_link_libraries(gta6rt_fexhost PRIVATE "-framework AppKit" "-framework QuartzCore" "-framework Foundation")
endif()
