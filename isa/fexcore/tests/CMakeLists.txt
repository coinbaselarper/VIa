set(_fex_guest_obj "${CMAKE_CURRENT_BINARY_DIR}/fex_guest_x86_64.o")
set(_fex_guest_hdr "${CMAKE_CURRENT_BINARY_DIR}/fex_guest_blob.h")
add_custom_command(OUTPUT "${_fex_guest_obj}"
  COMMAND "${CMAKE_C_COMPILER}" -arch x86_64 -c "${CMAKE_CURRENT_SOURCE_DIR}/fex_guest.S" -o "${_fex_guest_obj}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fex_guest.S" VERBATIM)
add_custom_command(OUTPUT "${_fex_guest_hdr}"
  COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/../tools/fex_blob.py" "${_fex_guest_obj}" "${_fex_guest_hdr}"
  DEPENDS "${_fex_guest_obj}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/fex_blob.py" VERBATIM)
add_custom_target(isa_fex_guest_blob DEPENDS "${_fex_guest_hdr}")

add_executable(isa_fex_test isa_fex_test.cpp)
add_dependencies(isa_fex_test isa_fex_guest_blob)
target_include_directories(isa_fex_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../../common")
target_link_libraries(isa_fex_test PRIVATE gta6rt_isa gta6rt_build_options)
target_compile_definitions(isa_fex_test PRIVATE GTA6RT_ISA_TEST_EXE="${GTA6RT_ROOT}/../exe/game_scarlett_master.exe")

# The same guest kernels linked natively into an x86_64 executable, for the Rosetta side of the comparison.
set(_fex_bench_x86 "${CMAKE_CURRENT_BINARY_DIR}/isa_fex_bench_x86")
add_custom_command(OUTPUT "${_fex_bench_x86}"
  COMMAND "${CMAKE_C_COMPILER}" -arch x86_64 -O2 -o "${_fex_bench_x86}" "${CMAKE_CURRENT_SOURCE_DIR}/bench_x86.c" "${_fex_guest_obj}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/bench_x86.c" "${_fex_guest_obj}" VERBATIM)
add_custom_target(isa_fex_bench_x86_build DEPENDS "${_fex_bench_x86}")

# The Darwin futex shim (third_party/fex/compat) against FEX's own lock and condition variable, host threads only.
add_executable(fex_futex_test fex_futex_test.cpp)
target_link_libraries(fex_futex_test PRIVATE gta6rt_fex gta6rt_build_options)
target_compile_definitions(fex_futex_test PRIVATE GTA6RT_ISA_TEST_EXE="${GTA6RT_ROOT}/../exe/game_scarlett_master.exe")
