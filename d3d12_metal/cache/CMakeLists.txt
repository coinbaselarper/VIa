cmake_minimum_required(VERSION 3.24)

# Standalone tools and tests of the Metal path's pipeline cache:
#   cmake -S port/d3d12_metal/cache -B port/build/pcache-<arch> -G Ninja -DCMAKE_OSX_ARCHITECTURES=<arch>
#   ninja -C port/build/pcache-<arch> && ctest --test-dir port/build/pcache-<arch>
# prepare_shaders must be the arch the title runs as (x86_64): the archive key includes the process arch.

project(gta6rt_pcache LANGUAGES C CXX OBJCXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
enable_testing()

get_filename_component(_port "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_subdirectory("${_port}/metal" "${CMAKE_BINARY_DIR}/metal")

add_library(gta6rt_pcache STATIC src/codec.cpp src/pcache.cpp src/phase_standalone.cpp)
target_include_directories(gta6rt_pcache PUBLIC include PRIVATE src "${_port}/include")
target_link_libraries(gta6rt_pcache PUBLIC gta6rt_metal)
target_compile_options(gta6rt_pcache PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_options(gta6rt_pcache INTERFACE "LINKER:-U,_gta6rt_governor_level" "LINKER:-U,_gta6rt_governor_register")

add_library(gta6rt_pcache_catalog STATIC "${_port}/shaderc_msl/src/catalog.cpp")
target_include_directories(gta6rt_pcache_catalog PUBLIC "${_port}/shaderc_msl/include")

add_executable(prepare_shaders "${_port}/d3d12_metal/tools/prepare_shaders.cpp")
target_link_libraries(prepare_shaders PRIVATE gta6rt_pcache gta6rt_pcache_catalog)
target_compile_definitions(prepare_shaders PRIVATE GTA6RT_PORT_ROOT="${_port}")

add_executable(pcache_codec_test tests/codec_test.cpp)
target_link_libraries(pcache_codec_test PRIVATE gta6rt_pcache)
target_compile_definitions(pcache_codec_test PRIVATE GTA6RT_TEST_PORT_ROOT="${_port}")
add_test(NAME pcache_codec COMMAND pcache_codec_test "${CMAKE_BINARY_DIR}/test-tmp")

add_executable(pcache_runtime_test tests/runtime_test.cpp)
target_link_libraries(pcache_runtime_test PRIVATE gta6rt_pcache gta6rt_pcache_catalog)
target_compile_definitions(pcache_runtime_test PRIVATE GTA6RT_PORT_ROOT="${_port}")
add_test(NAME pcache_runtime COMMAND pcache_runtime_test "${CMAKE_BINARY_DIR}/test-tmp/runtime")
set_tests_properties(pcache_runtime PROPERTIES TIMEOUT 300)

add_executable(pcache_archive_test tests/archive_test.cpp)
target_link_libraries(pcache_archive_test PRIVATE gta6rt_pcache gta6rt_pcache_catalog)
target_compile_definitions(pcache_archive_test PRIVATE GTA6RT_PORT_ROOT="${_port}")
add_test(NAME pcache_archive COMMAND pcache_archive_test "${CMAKE_BINARY_DIR}/test-tmp/archive")
set_tests_properties(pcache_archive PROPERTIES TIMEOUT 300)
