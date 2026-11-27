# Private super-build hook for the D3D12 -> gta6rt::gpu (Metal) layer.
#
#   cmake -S port -B port/build/d3d12_metal-<arch> -G Ninja -DCMAKE_OSX_ARCHITECTURES=<arch> \
#         -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=<port>/d3d12_metal/superbuild/attach.cmake
#
# port/CMakeLists.txt and every module are configured unchanged. After the top level is processed this adds
# port/d3d12_metal and applies the host change the integrator would make by hand (STATUS.md "Host change"): the
# host force-loads gta6rt_d3d12_metal and registers d3d12_xs.dll through gta6rt_d3d12_backend_register(), which
# picks the backend from GTA6RT_GPU (vulkan by default, metal on request). host/src/main.cpp is not edited: a copy
# patched at configure time replaces it in this tree's gta6rt target, and configuring fails if the patch no longer
# applies.

get_filename_component(_d3d12_metal_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(D3D12_METAL_ROOT "${_d3d12_metal_root}" CACHE INTERNAL "")

function(_d3d12_metal_attach)
  if(NOT TARGET gta6rt_d3d12_vulkan)
    message(FATAL_ERROR "d3d12_metal needs the d3d12_vulkan module configured (its front end and generated headers)")
  endif()
  set(D3D12MTL_BIN "${CMAKE_BINARY_DIR}/d3d12_metal")
  file(MAKE_DIRECTORY "${D3D12MTL_BIN}")
  include("${D3D12_METAL_ROOT}/d3d12_metal.cmake")
  if(NOT TARGET gta6rt)
    message(STATUS "d3d12_metal: no host target in this tree; library only")
    return()
  endif()

  set(_main "${GTA6RT_ROOT}/host/src/main.cpp")
  file(READ "${_main}" _text)
  set(_call "d3d_st = gta6rt_d3d12_vulkan_register();")
  string(FIND "${_text}" "${_call}" _at)
  if(_at LESS 0)
    message(FATAL_ERROR "d3d12_metal: host/src/main.cpp no longer contains '${_call}'; update attach.cmake")
  endif()
  string(REPLACE "${_call}" "d3d_st = gta6rt_d3d12_backend_register();" _text "${_text}")
  set(_exit_call "gta6rt_d3d12_vulkan_at_exit(2000)")
  string(FIND "${_text}" "${_exit_call}" _exit_at)
  if(_exit_at LESS 0)
    message(FATAL_ERROR "d3d12_metal: host/src/main.cpp no longer contains '${_exit_call}'; update attach.cmake")
  endif()
  string(REPLACE "${_exit_call}" "gta6rt_d3d12_backend_at_exit(2000)" _text "${_text}")
  string(REPLACE "#include <gta6rt/shims.h>" "#include <gta6rt/shims.h>\n#include <gta6rt/d3d12_backend.h>" _text "${_text}")
  string(FIND "${_text}" "gta6rt/d3d12_backend.h" _inc)
  if(_inc LESS 0)
    message(FATAL_ERROR "d3d12_metal: host/src/main.cpp has no '#include <gta6rt/shims.h>' to put the backend header after")
  endif()
  set(_patched "${CMAKE_BINARY_DIR}/d3d12_metal/host_main_patched.cpp")
  file(CONFIGURE OUTPUT "${_patched}" CONTENT "${_text}" @ONLY)
  set_property(DIRECTORY "${GTA6RT_ROOT}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_main}")

  get_target_property(_sources gta6rt SOURCES)
  list(TRANSFORM _sources REPLACE "^src/main\\.cpp$" "${_patched}")
  list(FIND _sources "${_patched}" _found)
  if(_found LESS 0)
    message(FATAL_ERROR "d3d12_metal: gta6rt has no src/main.cpp source to replace (${_sources})")
  endif()
  set_property(TARGET gta6rt PROPERTY SOURCES ${_sources})
  set_source_files_properties("${_patched}" TARGET_DIRECTORY gta6rt PROPERTIES
    COMPILE_OPTIONS "-iquote;${GTA6RT_ROOT}/host/src")
  target_link_libraries(gta6rt PRIVATE gta6rt_d3d12_select "$<LINK_LIBRARY:WHOLE_ARCHIVE,gta6rt_d3d12_metal>")
  message(STATUS "d3d12_metal: host gta6rt links gta6rt_d3d12_metal; GTA6RT_GPU=metal selects it")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _d3d12_metal_attach)
