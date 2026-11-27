# arm64 host: guest x86-64 code runs inside the vendored libFEXCore (third_party/fex, X6 spike).
include("${GTA6RT_THIRD_PARTY}/fex/fex.cmake")

add_library(gta6rt_isa_backend OBJECT fexcore_backend.cpp fexcore_native.cpp fexcore_asm.S)
target_include_directories(gta6rt_isa_backend PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../common")
target_link_libraries(gta6rt_isa_backend PRIVATE gta6rt_interfaces gta6rt_build_options gta6rt_fex gta6rt_os)
target_compile_options(gta6rt_isa_backend PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-strict-aliasing>)

# gta6rt_isa is created by the parent directory after this one returns; the FEX archives must reach everything that
# links it (static library: link-only propagation).
cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.." CALL target_link_libraries gta6rt_isa PRIVATE gta6rt_fex)

add_subdirectory(tests)

# gta6rt_fexhost (host/src with host_integration/*.patch applied) is a private pre-integration host for isa/fexcore
# runs. Off by default so a host/src change can never break another arm64 tree through a stale patch.
option(GTA6RT_FEX_HOST "Build gta6rt_fexhost (isa/fexcore pre-integration host; needs GTA6RT_MODULES=all)" OFF)
if(GTA6RT_FEX_HOST AND GTA6RT_MODULES STREQUAL "all")
  add_subdirectory(host_integration)
endif()
