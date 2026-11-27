cmake_minimum_required(VERSION 3.24)
project(gta6rt_io_bench LANGUAGES C CXX)

if(NOT APPLE)
  message(FATAL_ERROR "io_bench measures the macOS file IO backend")
endif()
list(LENGTH CMAKE_OSX_ARCHITECTURES _iob_n_arch)
if(_iob_n_arch GREATER 1)
  message(FATAL_ERROR "set at most one CMAKE_OSX_ARCHITECTURES (x86_64 or arm64) per build tree")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_package(ZLIB REQUIRED)

set(IOB_PORTABLE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../portable" CACHE PATH
    "port/portable (read only): leonida's zlib_inflate and BCPack decoder; empty disables the codec benchmarks of leonida's code")
set(IOB_LIBDEFLATE_DIR "" CACHE PATH "optional libdeflate source tree (MIT) for the optimised inflate comparison")

add_executable(iobench
  src/main.cpp
  src/shapes.cpp
  src/reader.cpp
  src/codec.cpp
  src/model.cpp
  src/pressure.cpp)
target_link_libraries(iobench PRIVATE ZLIB::ZLIB)
target_compile_options(iobench PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -fno-omit-frame-pointer)

if(IOB_PORTABLE_DIR AND EXISTS "${IOB_PORTABLE_DIR}/src/shader_library.h" AND EXISTS "${IOB_PORTABLE_DIR}/tools/gen_bcpack_runtime_tables.py")
  set(_iob_tables "${CMAKE_CURRENT_BINARY_DIR}/generated/bcpack_tables.inc")
  add_custom_command(OUTPUT "${_iob_tables}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
            "${Python3_EXECUTABLE}" -I -B "${IOB_PORTABLE_DIR}/tools/gen_bcpack_runtime_tables.py" "${_iob_tables}"
    DEPENDS "${IOB_PORTABLE_DIR}/tools/gen_bcpack_runtime_tables.py"
            "${IOB_PORTABLE_DIR}/tools/bcpack.py"
            "${IOB_PORTABLE_DIR}/tools/bcpack_tables.py"
            "${IOB_PORTABLE_DIR}/tools/bcpack_tables_bptc.py"
    VERBATIM)
  add_library(iob_leonida_codecs STATIC "${IOB_PORTABLE_DIR}/src/bcpack.cpp" "${_iob_tables}")
  target_include_directories(iob_leonida_codecs PUBLIC "${IOB_PORTABLE_DIR}/src")
  target_include_directories(iob_leonida_codecs PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
  target_link_libraries(iobench PRIVATE iob_leonida_codecs)
  target_compile_definitions(iobench PRIVATE IOB_HAVE_LEONIDA=1)
  message(STATUS "io_bench: leonida codecs from ${IOB_PORTABLE_DIR}")
else()
  message(STATUS "io_bench: leonida codecs disabled (IOB_PORTABLE_DIR not usable)")
endif()

if(IOB_LIBDEFLATE_DIR AND EXISTS "${IOB_LIBDEFLATE_DIR}/libdeflate.h")
  set(_iob_ld_all
    "${IOB_LIBDEFLATE_DIR}/lib/adler32.c"
    "${IOB_LIBDEFLATE_DIR}/lib/deflate_decompress.c"
    "${IOB_LIBDEFLATE_DIR}/lib/utils.c"
    "${IOB_LIBDEFLATE_DIR}/lib/zlib_decompress.c")
  if(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
    list(APPEND _iob_ld_all "${IOB_LIBDEFLATE_DIR}/lib/x86/cpu_features.c")
  else()
    list(APPEND _iob_ld_all "${IOB_LIBDEFLATE_DIR}/lib/arm/cpu_features.c")
  endif()
  add_library(iob_libdeflate STATIC ${_iob_ld_all})
  target_include_directories(iob_libdeflate PUBLIC "${IOB_LIBDEFLATE_DIR}" PRIVATE "${IOB_LIBDEFLATE_DIR}/lib")
  target_compile_options(iob_libdeflate PRIVATE -O2 -w)
  target_link_libraries(iobench PRIVATE iob_libdeflate)
  target_compile_definitions(iobench PRIVATE IOB_HAVE_LIBDEFLATE=1)
  message(STATUS "io_bench: libdeflate from ${IOB_LIBDEFLATE_DIR}")
endif()

set(IOB_BCPACK_HISTORY "${CMAKE_CURRENT_SOURCE_DIR}/../build/shaders_vk/apple/bcpack/history.bcph" CACHE FILEPATH
    "where the DirectStorage shim looks for the BCPack predictor profile (<shader folder parent>/bcpack/history.bcph)")
add_custom_target(bcpack_history
  COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/scripts/ensure_bcpack_history.sh" --out "${IOB_BCPACK_HISTORY}"
  COMMENT "BCPack predictor profile: generate when missing (leonida/tools/build_dstorage_history.py)"
  USES_TERMINAL VERBATIM)
