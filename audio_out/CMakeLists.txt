# gta6rt_audio_out: CoreAudio output for the title's render endpoint (see STATUS.md).
# Standalone: cmake -S port/audio_out -B port/build/audio_out-<arch> -G Ninja -DCMAKE_OSX_ARCHITECTURES=<arch>
# Under port/: add_subdirectory(audio_out) before shims_gdk.
cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(gta6rt_audio_out LANGUAGES C CXX)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  set(_audio_out_default_tests ON)
else()
  set(_audio_out_default_tests OFF)
endif()
option(GTA6RT_AUDIO_OUT_TESTS "Build the gta6rt_audio_out tests and tools" ${_audio_out_default_tests})

add_library(gta6rt_audio_out STATIC
  src/audio_out.cpp
  src/device_coreaudio.cpp
  src/downmix.cpp
  src/opus.cpp
  src/wav.cpp)
target_include_directories(gta6rt_audio_out PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(gta6rt_audio_out PUBLIC cxx_std_20)
target_compile_options(gta6rt_audio_out PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(gta6rt_audio_out PUBLIC "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreFoundation")
if(TARGET gta6rt_build_options)
  target_link_libraries(gta6rt_audio_out PRIVATE gta6rt_build_options)
endif()

if(GTA6RT_AUDIO_OUT_TESTS)
  foreach(_t downmix stream device src opus)
    add_executable(audio_out_${_t}_test tests/${_t}_test.cpp)
    target_include_directories(audio_out_${_t}_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_options(audio_out_${_t}_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(audio_out_${_t}_test PRIVATE gta6rt_audio_out)
  endforeach()
  add_executable(audio_out_wavstat tools/wavstat.cpp)
  target_include_directories(audio_out_wavstat PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(audio_out_wavstat PRIVATE gta6rt_audio_out)
endif()
