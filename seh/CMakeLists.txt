# S1.3 .pdata unwinder and dispatcher. Implements include/gta6rt/seh.h and the Rtl* functions in compat/winnt.h.
set(_seh_sources
  src/api.cpp
  src/tables.cpp
  src/unwind.cpp
  src/dispatch.cpp
  src/fault.cpp)
if(GTA6RT_ARCH STREQUAL "x86_64")
  list(APPEND _seh_sources src/seh_x86_64.S)
else()
  list(APPEND _seh_sources src/arch_other.cpp)
endif()
add_library(gta6rt_seh STATIC ${_seh_sources})
target_link_libraries(gta6rt_seh PUBLIC gta6rt_interfaces PRIVATE gta6rt_wincompat gta6rt_isa gta6rt_os)

add_subdirectory(tests)
