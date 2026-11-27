# Worker build trees for gta6rt_media without editing port/CMakeLists.txt:
#   cmake -S port/media/dev -B port/build/media-x86_64 -G Ninja -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_BUILD_TYPE=RelWithDebInfo
# It configures port/ with GTA6RT_MODULES (default os;isa;seh;loader;shims_core;shims_gdk) and adds port/media.
cmake_minimum_required(VERSION 3.24)
project(gta6rt_media_dev LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

get_filename_component(_port "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if(NOT DEFINED GTA6RT_MODULES)
  set(GTA6RT_MODULES "os;isa;seh;loader;shims_core;shims_gdk" CACHE STRING "port modules to configure")
endif()
add_subdirectory("${_port}" port)
add_subdirectory("${_port}/media" media)

# With -DGTA6RT_MODULES=all the port's host executable exists; this links gta6rt_media into it and registers the three
# DLLs from a constructor, so a worker host can run before host/src/main.cpp calls gta6rt_media_register().
option(GTA6RT_MEDIA_HOST_AUTOREGISTER "Link gta6rt_media into the gta6rt host and register it at load" OFF)
if(GTA6RT_MEDIA_HOST_AUTOREGISTER AND TARGET gta6rt)
  target_sources(gta6rt PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/autoregister.cpp")
  target_link_libraries(gta6rt PRIVATE gta6rt_media)
  # The window layer's sources (host/src/win) are not in host/CMakeLists.txt yet; its own overlay adds them.
  file(READ "${_port}/host/CMakeLists.txt" _host_cmake)
  if(EXISTS "${_port}/host/src/win/win.cmake" AND NOT _host_cmake MATCHES "win\\.cmake")
    include("${_port}/host/src/win/win.cmake")
  endif()
endif()
