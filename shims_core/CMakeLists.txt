# S1.5a POSIX ports of the core shims (registry, process, lifecycle, library_loader, file_system, winhttp/ws2
# offline, bcrypt_rng, xmem, xg_xs, com_shim, xbox_bvh_builder), the export table (include/gta6rt/shims.h) and the
# trivial kernel32/ucrt imports. Link the archive with -force_load from host/ (replaces /OPT:NOREF).
set(SHIMS_CORE_SOURCES
  src/registry.cpp
  src/api.cpp
  src/crt_string.cpp
  src/crt_math.cpp
  src/crt_convert.cpp
  src/crt_stdio.cpp
  src/crt_runtime.cpp
  src/crt_time.cpp
  src/crt_extra.cpp
  src/k32_misc.cpp
  src/process.cpp
  src/file_system.cpp
  src/console_registry.cpp
  src/bcrypt_rng.cpp
  src/com_shim.cpp
  src/lifecycle.cpp
  src/library_loader.cpp
  src/winhttp_offline.cpp
  src/ws2_offline.cpp
  src/xg_xs.cpp
  src/xbox_bvh_builder.cpp
  src/xmem.cpp
)

add_library(gta6rt_shims_core_objs OBJECT ${SHIMS_CORE_SOURCES})
target_include_directories(gta6rt_shims_core_objs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(gta6rt_shims_core_objs PUBLIC gta6rt_interfaces gta6rt_dxheaders gta6rt_dxguids gta6rt_wincompat gta6rt_build_options)
target_compile_options(gta6rt_shims_core_objs PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_library(gta6rt_shims_core STATIC $<TARGET_OBJECTS:gta6rt_shims_core_objs>)
target_include_directories(gta6rt_shims_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(gta6rt_shims_core PUBLIC gta6rt_interfaces PRIVATE gta6rt_wincompat gta6rt_dxguids)
foreach(_dep gta6rt_isa gta6rt_os)
  if(TARGET ${_dep})
    target_link_libraries(gta6rt_shims_core PRIVATE ${_dep})
  endif()
endforeach()
if(TARGET gta6rt_os)
  target_sources(gta6rt_shims_core PRIVATE src/os_bindings.cpp)
  target_link_libraries(gta6rt_shims_core PRIVATE gta6rt_build_options)
  target_compile_options(gta6rt_shims_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# shims_core_test runs against tests/env_stub.cpp (a libc-backed stand-in for the os.h/isa.h subset the shims call).
# GTA6RT_SHIMS_CORE_TEST_REAL_ENV=ON builds shims_core_real_smoke instead: the registry resolver and a few shims driven
# through real gta6rt_os/gta6rt_isa thunks (x86_64/rosetta only).
option(GTA6RT_SHIMS_CORE_TESTS "build the shims_core tests" ON)
option(GTA6RT_SHIMS_CORE_TEST_REAL_ENV "link the tests against gta6rt_os/gta6rt_isa instead of tests/env_stub.cpp" OFF)
if(GTA6RT_SHIMS_CORE_TESTS)
  if(GTA6RT_SHIMS_CORE_TEST_REAL_ENV)
    add_executable(shims_core_real_smoke tests/real_env_smoke.cpp)
    target_link_libraries(shims_core_real_smoke PRIVATE gta6rt_build_options gta6rt_wincompat gta6rt_isa gta6rt_os)
    target_link_options(shims_core_real_smoke PRIVATE "LINKER:-force_load,$<TARGET_FILE:gta6rt_shims_core>")
    target_link_libraries(shims_core_real_smoke PRIVATE gta6rt_shims_core)
    add_executable(shims_core_import_bench tests/import_bench.cpp)
    target_link_libraries(shims_core_import_bench PRIVATE gta6rt_build_options gta6rt_wincompat gta6rt_isa gta6rt_os)
    target_link_options(shims_core_import_bench PRIVATE "LINKER:-force_load,$<TARGET_FILE:gta6rt_shims_core>")
    target_link_libraries(shims_core_import_bench PRIVATE gta6rt_shims_core)
    target_include_directories(shims_core_import_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  else()
    add_executable(shims_core_test tests/main.cpp tests/test_registry.cpp tests/test_crt.cpp tests/test_coverage.cpp tests/test_k32.cpp tests/test_process_run.cpp tests/test_fs.cpp tests/test_services.cpp tests/test_ws2.cpp tests/test_modules.cpp tests/test_xg.cpp tests/test_bvh.cpp tests/test_api.cpp tests/env_stub.cpp)
    target_link_libraries(shims_core_test PRIVATE gta6rt_shims_core_objs)
    if(GTA6RT_ARCH STREQUAL "x86_64")
      target_link_options(shims_core_test PRIVATE "LINKER:-pagezero_size,0x10000")
    endif()
    foreach(_t bcrypt_rng_test)
      add_executable(leonida_${_t} tests/leonida/${_t}.cpp tests/env_stub.cpp)
      target_link_libraries(leonida_${_t} PRIVATE gta6rt_shims_core_objs)
    endforeach()
  endif()
endif()
