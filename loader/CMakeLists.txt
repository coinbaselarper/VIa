# S1.4 PE loader. Implements include/gta6rt/loader.h.
add_library(gta6rt_loader STATIC
  src/host_ops.cpp
  src/pe_map.cpp
  src/pe_imports.cpp
  src/tls.cpp
  src/package.cpp
  src/diag.cpp)
target_link_libraries(gta6rt_loader PUBLIC gta6rt_interfaces
  PRIVATE gta6rt_wincompat gta6rt_build_options gta6rt_seh gta6rt_isa gta6rt_os)

add_library(gta6rt_loader_testsupport STATIC tests/posix_ops.cpp)
target_link_libraries(gta6rt_loader_testsupport PUBLIC gta6rt_loader gta6rt_interfaces PRIVATE gta6rt_wincompat)

add_executable(loader_pe_stats tests/pe_stats.cpp)
target_compile_definitions(loader_pe_stats PRIVATE GTA6RT_LOADER_TEST_ROOT="${GTA6RT_ROOT}/..")
target_link_libraries(loader_pe_stats PRIVATE gta6rt_loader_testsupport gta6rt_wincompat)

add_executable(loader_selftest tests/selftest.cpp)
target_link_libraries(loader_selftest PRIVATE gta6rt_loader_testsupport gta6rt_wincompat)
