cmake_minimum_required(VERSION 3.24)
project(gta6rt_xma LANGUAGES C CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_gta6rt_xma_standalone ON)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
  endif()
else()
  set(_gta6rt_xma_standalone OFF)
endif()

option(GTA6RT_XMA_TESTS "Build the gta6rt_xma tests and tools" ${_gta6rt_xma_standalone})
set(GTA6RT_XMA_DECODER_LIB "" CACHE FILEPATH "Prebuilt libgta6rt_xma_decoder.a for this arch (skips cargo)")

list(LENGTH CMAKE_OSX_ARCHITECTURES _xma_narch)
if(_xma_narch EQUAL 0)
  set(_xma_arch "${CMAKE_HOST_SYSTEM_PROCESSOR}")
elseif(_xma_narch EQUAL 1)
  set(_xma_arch "${CMAKE_OSX_ARCHITECTURES}")
else()
  message(FATAL_ERROR "gta6rt_xma: set one CMAKE_OSX_ARCHITECTURES (x86_64 or arm64) per build tree")
endif()
if(_xma_arch STREQUAL "x86_64")
  set(_xma_rust_target x86_64-apple-darwin)
elseif(_xma_arch MATCHES "^(arm64|aarch64)$")
  set(_xma_rust_target aarch64-apple-darwin)
else()
  message(FATAL_ERROR "gta6rt_xma: unsupported architecture ${_xma_arch}")
endif()

if(GTA6RT_XMA_DECODER_LIB)
  if(NOT EXISTS "${GTA6RT_XMA_DECODER_LIB}")
    message(FATAL_ERROR "GTA6RT_XMA_DECODER_LIB=${GTA6RT_XMA_DECODER_LIB} does not exist")
  endif()
  set(_xma_lib "${GTA6RT_XMA_DECODER_LIB}")
  add_custom_target(gta6rt_xma_decoder_build)
else()
  find_program(GTA6RT_CARGO cargo HINTS "$ENV{HOME}/.cargo/bin" REQUIRED)
  set(_xma_cargo_dir "${CMAKE_CURRENT_BINARY_DIR}/cargo")
  set(_xma_lib "${_xma_cargo_dir}/${_xma_rust_target}/release/libgta6rt_xma_decoder.a")
  file(GLOB _xma_rust_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/rust/src/*.rs" "${CMAKE_CURRENT_SOURCE_DIR}/third_party/xma/src/*.rs")
  if(CMAKE_OSX_DEPLOYMENT_TARGET)
    set(_xma_deploy "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
  else()
    set(_xma_deploy "MACOSX_DEPLOYMENT_TARGET=12.0")
  endif()
  add_custom_command(
    OUTPUT "${_xma_lib}"
    COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${_xma_cargo_dir}" ${_xma_deploy}
            "${GTA6RT_CARGO}" build --release --offline --locked --quiet
            --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml" --target ${_xma_rust_target}
    DEPENDS ${_xma_rust_sources} "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml" "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.lock"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/xma/Cargo.toml"
    COMMENT "cargo: gta6rt_xma_decoder for ${_xma_rust_target}"
    VERBATIM)
  add_custom_target(gta6rt_xma_decoder_build DEPENDS "${_xma_lib}")
endif()

add_library(gta6rt_xma_decoder STATIC IMPORTED GLOBAL)
set_target_properties(gta6rt_xma_decoder PROPERTIES IMPORTED_LOCATION "${_xma_lib}")
add_dependencies(gta6rt_xma_decoder gta6rt_xma_decoder_build)

add_library(gta6rt_xma STATIC src/engine.cpp)
add_dependencies(gta6rt_xma gta6rt_xma_decoder_build)
target_include_directories(gta6rt_xma PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(gta6rt_xma PUBLIC cxx_std_20)
target_compile_options(gta6rt_xma PRIVATE -Wall -Wextra)
target_compile_definitions(gta6rt_xma INTERFACE GTA6RT_HAVE_XMA=1)
target_link_libraries(gta6rt_xma PUBLIC gta6rt_xma_decoder)

if(GTA6RT_XMA_TESTS)
  foreach(_t xma_engine_test)
    add_executable(${_t} tests/${_t}.cpp)
    target_include_directories(${_t} PRIVATE tests)
    target_link_libraries(${_t} PRIVATE gta6rt_xma)
  endforeach()
  foreach(_t xma_decode xma_bench)
    add_executable(${_t} tools/${_t}.cpp)
    target_include_directories(${_t} PRIVATE tests)
    target_link_libraries(${_t} PRIVATE gta6rt_xma)
  endforeach()
endif()

message(STATUS "gta6rt_xma: arch=${_xma_arch} rust=${_xma_rust_target} decoder=${_xma_lib} tests=${GTA6RT_XMA_TESTS}")
