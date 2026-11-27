cmake_minimum_required(VERSION 3.24)
project(gta6rt_port LANGUAGES C CXX ASM)

# One architecture per build tree: -DCMAKE_OSX_ARCHITECTURES=x86_64 (Rosetta host, shipped) or arm64 (escape route).

if(NOT APPLE)
  message(FATAL_ERROR "gta6rt port is the macOS host")
endif()
list(LENGTH CMAKE_OSX_ARCHITECTURES _gta6rt_n_arch)
if(NOT _gta6rt_n_arch EQUAL 1)
  message(FATAL_ERROR "set exactly one CMAKE_OSX_ARCHITECTURES (x86_64 or arm64) per build tree")
endif()
set(GTA6RT_ARCH "${CMAKE_OSX_ARCHITECTURES}")
if(NOT GTA6RT_ARCH MATCHES "^(x86_64|arm64)$")
  message(FATAL_ERROR "unsupported CMAKE_OSX_ARCHITECTURES=${GTA6RT_ARCH}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(GTA6RT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(GTA6RT_THIRD_PARTY "${GTA6RT_ROOT}/third_party")

if(GTA6RT_ARCH STREQUAL "x86_64")
  set(_gta6rt_default_isa rosetta)
else()
  set(_gta6rt_default_isa fexcore)
endif()
set(GTA6RT_ISA_BACKEND "${_gta6rt_default_isa}" CACHE STRING "ISA backend: rosetta (x86_64) or fexcore (arm64)")
if(GTA6RT_ISA_BACKEND STREQUAL "rosetta" AND NOT GTA6RT_ARCH STREQUAL "x86_64")
  message(FATAL_ERROR "isa backend rosetta needs CMAKE_OSX_ARCHITECTURES=x86_64")
endif()
if(GTA6RT_ISA_BACKEND STREQUAL "fexcore" AND NOT GTA6RT_ARCH STREQUAL "arm64")
  message(FATAL_ERROR "isa backend fexcore needs CMAKE_OSX_ARCHITECTURES=arm64")
endif()

# Modules to configure. Workers may restrict this to their own module (plus its dependencies) so that a
# half-edited CMakeLists.txt elsewhere does not break their configure. host/ is only added when all are present.
set(GTA6RT_ALL_MODULES os isa seh loader shims_core shims_gdk media d3d12_vulkan host)
set(GTA6RT_MODULES "all" CACHE STRING "Semicolon list of modules to configure, or 'all'")
if(GTA6RT_MODULES STREQUAL "all")
  set(_gta6rt_modules ${GTA6RT_ALL_MODULES})
else()
  set(_gta6rt_modules ${GTA6RT_MODULES})
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_library(gta6rt_interfaces INTERFACE)
target_include_directories(gta6rt_interfaces INTERFACE "${GTA6RT_ROOT}/include")

# Common options for everything we build. No _WIN32, ever.
add_library(gta6rt_build_options INTERFACE)
target_compile_definitions(gta6rt_build_options INTERFACE
  GTA6RT_HOST_ARCH_${GTA6RT_ARCH}=1
  $<$<STREQUAL:${GTA6RT_ISA_BACKEND},rosetta>:GTA6RT_ISA_BACKEND_ROSETTA=1>
  $<$<STREQUAL:${GTA6RT_ISA_BACKEND},fexcore>:GTA6RT_ISA_BACKEND_FEXCORE=1>)
target_compile_options(gta6rt_build_options INTERFACE -fno-omit-frame-pointer)
target_link_libraries(gta6rt_build_options INTERFACE gta6rt_interfaces)

add_subdirectory(compat)
add_subdirectory(portable)
add_subdirectory(audio_out)
# SHAPE XMA decode on the host (port/xma): Rust decoder built with cargo per arch, linked into shims_gdk.
add_subdirectory(xma)

# Metal backend (port/metal, its own worker). Not linked into the host yet; built so the tree keeps it compiling.
option(GTA6RT_WITH_METAL "Configure port/metal (tests off unless GTA6RT_METAL_TESTS)" ON)
if(GTA6RT_WITH_METAL)
  set(GTA6RT_METAL_TESTS OFF CACHE BOOL "Build the gta6rt_metal host tests")
  add_subdirectory(metal)
endif()

foreach(_m IN LISTS GTA6RT_ALL_MODULES)
  if(_m STREQUAL "host")
    continue()
  endif()
  if(_m IN_LIST _gta6rt_modules)
    add_subdirectory(${_m})
  endif()
endforeach()

set(_gta6rt_have_all ON)
foreach(_m IN LISTS GTA6RT_ALL_MODULES)
  if(NOT _m IN_LIST _gta6rt_modules)
    set(_gta6rt_have_all OFF)
  endif()
endforeach()
if(_gta6rt_have_all)
  add_subdirectory(host)
endif()

message(STATUS "gta6rt: arch=${GTA6RT_ARCH} isa=${GTA6RT_ISA_BACKEND} modules=${_gta6rt_modules}")
