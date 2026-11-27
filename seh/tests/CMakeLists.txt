add_executable(seh_unit_test unit_test.cpp)
target_link_libraries(seh_unit_test PRIVATE gta6rt_seh gta6rt_isa gta6rt_os gta6rt_wincompat)

if(GTA6RT_ARCH STREQUAL "x86_64")
  add_executable(seh_pdata_exec_test pdata_exec_test.cpp enter_guest_x86_64.S)
  target_link_libraries(seh_pdata_exec_test PRIVATE gta6rt_seh gta6rt_isa gta6rt_os gta6rt_wincompat)

  # Guest fixture: MSVC-ABI COFF object (FH3 C++ EH + SEH scopes) from the same clang, linked in memory by the test.
  set(_fixture_obj "${CMAKE_CURRENT_BINARY_DIR}/guest_eh.obj")
  add_custom_command(OUTPUT "${_fixture_obj}"
    COMMAND "${CMAKE_CXX_COMPILER}" -target x86_64-pc-windows-msvc -c -O1 -fexceptions -fcxx-exceptions
            -fms-extensions -fno-stack-protector -ffunction-sections
            -o "${_fixture_obj}" "${CMAKE_CURRENT_SOURCE_DIR}/fixture/guest_eh.cpp"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fixture/guest_eh.cpp"
    COMMENT "guest fixture (x86_64-pc-windows-msvc COFF)")
  add_custom_target(seh_guest_fixture DEPENDS "${_fixture_obj}")

  add_executable(seh_dispatch_test dispatch_test.cpp)
  add_dependencies(seh_dispatch_test seh_guest_fixture)
  target_compile_definitions(seh_dispatch_test PRIVATE
    SEH_FIXTURE_OBJ="${_fixture_obj}"
    SEH_PACKAGE_DIR="${GTA6RT_ROOT}/../xbox/GTA6.Xbox.v1.0.2.1-CYBERLEEK"
    SEH_EXE="${GTA6RT_ROOT}/../exe/game_scarlett_master.exe")
  target_link_libraries(seh_dispatch_test PRIVATE gta6rt_seh gta6rt_isa gta6rt_os)
endif()
