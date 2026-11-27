# S1.1 Win32 primitive layer. Implements include/gta6rt/os.h and the kernel32 surface declared in compat/.
add_library(gta6rt_os STATIC
  src/core.cpp
  src/vm.cpp
  src/win_memory.cpp
  src/handles.cpp
  src/waits.cpp
  src/inmem_sync.cpp
  src/threads.cpp
  src/threadpool.cpp
  src/paths.cpp
  src/files.cpp
  src/time_sys.cpp
  src/abi_fixups.c)
target_link_libraries(gta6rt_os PUBLIC gta6rt_interfaces PRIVATE gta6rt_wincompat)
if(GTA6RT_ARCH STREQUAL "x86_64")
  target_compile_options(gta6rt_os PRIVATE -mcx16)
endif()

# The title's 32-bit XMem heap needs address space below 4 GiB, which the default 4 GiB __PAGEZERO leaves none of.
# Every x86_64 executable that links gta6rt_os gets a 64 KiB page zero. arm64 macOS refuses to exec one smaller than
# 4 GiB; there xmem serves those requests above 4 GiB (shims_core/STATUS.md).
option(GTA6RT_OS_LOW_PAGEZERO "link x86_64 executables that use gta6rt_os with -pagezero_size 0x10000" ON)
if(GTA6RT_ARCH STREQUAL "x86_64" AND GTA6RT_OS_LOW_PAGEZERO)
  target_link_options(gta6rt_os INTERFACE
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:LINKER:-pagezero_size,0x10000>")
endif()

foreach(_t vm sync threads files misc)
  add_executable(os_test_${_t} tests/test_${_t}.cpp)
  target_link_libraries(os_test_${_t} PRIVATE gta6rt_os gta6rt_wincompat)
  if(GTA6RT_ARCH STREQUAL "x86_64")
    target_compile_options(os_test_${_t} PRIVATE -mcx16)
  endif()
endforeach()
