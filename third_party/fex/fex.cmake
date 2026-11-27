# Vendored libFEXCore (arm64 only), included from isa/fexcore/CMakeLists.txt.
#
# FEX is a separate CMake project with its own options and generated headers, so it is configured and built in
# its own tree, ${CMAKE_BINARY_DIR}/third_party/fex-build. The first build runs at configure time, alone, with
# GTA6RT_FEX_JOBS parallel jobs (the project rule is at most 3), so it never overlaps the outer build. After that
# the outer build re-runs FEX's own ninja (a no-op unless the vendored sources change) through gta6rt_fex_libs.
#
# Result: INTERFACE target gta6rt_fex (static FEX libraries, include dirs, ARCHITECTURE_arm64=1).

if(NOT GTA6RT_ARCH STREQUAL "arm64")
  message(FATAL_ERROR "third_party/fex builds for arm64 only (GTA6RT_ARCH=${GTA6RT_ARCH})")
endif()

set(GTA6RT_FEX_JOBS 3 CACHE STRING "Parallel jobs for the vendored FEX build (project rule: at most 3)")
set(GTA6RT_FEX_BUILD_TYPE Release CACHE STRING "CMAKE_BUILD_TYPE of the vendored FEX build")

set(_fex_root "${CMAKE_CURRENT_LIST_DIR}")
set(_fex_src "${_fex_root}/src")
set(_fex_compat "${_fex_root}/compat")
set(_fex_bld "${CMAKE_BINARY_DIR}/third_party/fex-build")
set(_fex_targets FEXCore FEXCore_Base JemallocDummy fmt xxhash softfloat_3e cephes_128bit)
set(_fex_libs
  "${_fex_bld}/FEXCore/Source/libFEXCore.a"
  "${_fex_bld}/FEXCore/Source/libFEXCore_Base.a"
  "${_fex_bld}/FEXCore/Source/libJemallocDummy.a"
  "${_fex_bld}/External/fmt/libfmt.a"
  "${_fex_bld}/External/xxhash/cmake_unofficial/libxxhash.a"
  "${_fex_bld}/External/SoftFloat-3e/libsoftfloat_3e.a"
  "${_fex_bld}/External/cephes/libcephes_128bit.a")

set(_fex_flags "-I${_fex_compat} -include ${_fex_compat}/fex_darwin_compat.h")
set(_fex_configure_args
  -S "${_fex_src}" -B "${_fex_bld}" -G Ninja
  "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
  "-DCMAKE_BUILD_TYPE=${GTA6RT_FEX_BUILD_TYPE}"
  "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
  "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
  "-DCMAKE_OSX_ARCHITECTURES=arm64"
  "-DCMAKE_OSX_SYSROOT=${CMAKE_OSX_SYSROOT}"
  "-DCMAKE_C_FLAGS=${_fex_flags}"
  "-DCMAKE_CXX_FLAGS=${_fex_flags}"
  "-DFEX_DARWIN_COMPAT_DIR=${_fex_compat}"
  -DENABLE_LTO=OFF -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF -DENABLE_FEX_ALLOCATOR=OFF -DENABLE_CCACHE=OFF
  -DBUILD_TESTING=OFF -DBUILD_FEXCONFIG=OFF -DENABLE_OFFLINE_TELEMETRY=OFF -DTUNE_CPU=none
  -DENABLE_GDB_SYMBOLS=OFF -DOVERRIDE_VERSION=FEX-3ee2539-gta6rt -DOVERRIDE_HASH=3ee25399232a6431c77c6314c437ea523ec29d0c)

# Re-run the configure-time build whenever the vendored tree's inputs that we control change.
file(GLOB _fex_patches "${_fex_root}/patches/*.diff")
file(GLOB_RECURSE _fex_compat_files "${_fex_compat}/*")
set(_fex_stamp_input "${_fex_configure_args};${GTA6RT_FEX_JOBS}")
foreach(_f IN LISTS _fex_patches _fex_compat_files)
  file(SHA256 "${_f}" _h)
  string(APPEND _fex_stamp_input ";${_f}=${_h}")
endforeach()
string(SHA256 _fex_stamp_hash "${_fex_stamp_input}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_fex_patches} ${_fex_compat_files})

set(_fex_stamp "${_fex_bld}/gta6rt-fex.stamp")
set(_fex_have "")
if(EXISTS "${_fex_stamp}")
  file(READ "${_fex_stamp}" _fex_have)
endif()
set(_fex_all_libs ON)
foreach(_l IN LISTS _fex_libs)
  if(NOT EXISTS "${_l}")
    set(_fex_all_libs OFF)
  endif()
endforeach()

if(NOT _fex_have STREQUAL _fex_stamp_hash OR NOT _fex_all_libs)
  file(MAKE_DIRECTORY "${_fex_bld}")
  message(STATUS "gta6rt: configuring vendored FEX (${_fex_src}) in ${_fex_bld}")
  execute_process(COMMAND "${CMAKE_COMMAND}" ${_fex_configure_args}
    RESULT_VARIABLE _rc OUTPUT_FILE "${_fex_bld}/gta6rt-configure.log" ERROR_FILE "${_fex_bld}/gta6rt-configure.log")
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "gta6rt: FEX configure failed (rc ${_rc}), see ${_fex_bld}/gta6rt-configure.log")
  endif()
  message(STATUS "gta6rt: building vendored FEX with ${GTA6RT_FEX_JOBS} jobs (first time: several minutes)")
  execute_process(COMMAND "${CMAKE_COMMAND}" --build "${_fex_bld}" -j ${GTA6RT_FEX_JOBS} --target ${_fex_targets}
    RESULT_VARIABLE _rc OUTPUT_FILE "${_fex_bld}/gta6rt-build.log" ERROR_FILE "${_fex_bld}/gta6rt-build.log")
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "gta6rt: FEX build failed (rc ${_rc}), see ${_fex_bld}/gta6rt-build.log")
  endif()
  file(WRITE "${_fex_stamp}" "${_fex_stamp_hash}")
endif()

add_custom_target(gta6rt_fex_libs
  COMMAND "${CMAKE_COMMAND}" --build "${_fex_bld}" -j ${GTA6RT_FEX_JOBS} --target ${_fex_targets}
  BYPRODUCTS ${_fex_libs}
  COMMENT "vendored FEX (incremental)"
  VERBATIM USES_TERMINAL)

add_library(gta6rt_fex INTERFACE)
add_dependencies(gta6rt_fex gta6rt_fex_libs)
target_include_directories(gta6rt_fex SYSTEM INTERFACE
  "${_fex_compat}"
  "${_fex_src}/FEXCore/include"
  "${_fex_bld}/include"
  "${_fex_src}/External/fmt/include"
  "${_fex_src}/External/unordered_dense/include"
  "${_fex_src}/FEXHeaderUtils")
target_compile_definitions(gta6rt_fex INTERFACE ARCHITECTURE_arm64=1)
# FEX headers use Linux names inline (SYS_futex, MADV_*); the same force-include FEX itself is built with.
target_compile_options(gta6rt_fex INTERFACE "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${_fex_compat}/fex_darwin_compat.h>")
target_link_libraries(gta6rt_fex INTERFACE ${_fex_libs})
set(GTA6RT_FEX_SOURCE_DIR "${_fex_src}" CACHE INTERNAL "")
set(GTA6RT_FEX_BINARY_DIR "${_fex_bld}" CACHE INTERNAL "")
