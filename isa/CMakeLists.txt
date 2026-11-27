# S1.2 / X2 / X5. The only place allowed to contain Rosetta- or FEX-specific code.
# gta6rt_isa = common code + exactly one backend, chosen by GTA6RT_ISA_BACKEND (rosetta on x86_64, fexcore on arm64).
# The backend is an object library folded into gta6rt_isa, so consumers link one archive.
if(GTA6RT_ISA_BACKEND STREQUAL "rosetta")
  add_subdirectory(rosetta)
elseif(GTA6RT_ISA_BACKEND STREQUAL "fexcore")
  add_subdirectory(fexcore)
else()
  message(FATAL_ERROR "unknown GTA6RT_ISA_BACKEND=${GTA6RT_ISA_BACKEND}")
endif()

add_library(gta6rt_isa STATIC
  common/host_cpu.cpp
  common/isa_core.cpp
  common/cpuid_policy.cpp
  common/sse4a.cpp
  common/x86_patch.cpp
  common/zen_insn.cpp
  $<TARGET_OBJECTS:gta6rt_isa_backend>)
target_include_directories(gta6rt_isa PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/common")
target_link_libraries(gta6rt_isa PUBLIC gta6rt_interfaces gta6rt_os PRIVATE gta6rt_build_options)

add_subdirectory(tests)
