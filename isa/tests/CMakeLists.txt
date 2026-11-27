set(_isa_test_exe "${GTA6RT_ROOT}/../exe/game_scarlett_master.exe")

add_executable(isa_unit_test isa_unit_test.cpp)
target_link_libraries(isa_unit_test PRIVATE gta6rt_isa gta6rt_build_options)
target_compile_definitions(isa_unit_test PRIVATE GTA6RT_ISA_TEST_EXE="${_isa_test_exe}")

if(GTA6RT_ISA_BACKEND STREQUAL "rosetta")
  add_executable(isa_rosetta_test isa_rosetta_test.cpp isa_test_x86.S)
  target_link_libraries(isa_rosetta_test PRIVATE gta6rt_isa gta6rt_build_options)
  target_compile_definitions(isa_rosetta_test PRIVATE GTA6RT_ISA_TEST_EXE="${_isa_test_exe}")
  add_executable(isa_reexec_test isa_reexec_test.cpp)
  target_link_libraries(isa_reexec_test PRIVATE gta6rt_isa gta6rt_build_options)
  target_compile_definitions(isa_reexec_test PRIVATE GTA6RT_ISA_TEST_EXE="${_isa_test_exe}")
endif()
