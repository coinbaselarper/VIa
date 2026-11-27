# x86_64 host: guest code runs natively under Rosetta 2. TEB via SHARE (TSD slots 6/11/12), SWAP selectable. S0.1/S0.6.
add_library(gta6rt_isa_backend OBJECT rosetta_backend.cpp rosetta_asm.S zen_wait.cpp)
target_include_directories(gta6rt_isa_backend PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../common")
target_link_libraries(gta6rt_isa_backend PRIVATE gta6rt_interfaces gta6rt_build_options)

add_executable(isa_zen_wait_test zen_wait_test.cpp)
target_link_libraries(isa_zen_wait_test PRIVATE gta6rt_isa gta6rt_build_options)
add_executable(isa_vshift_check_test vshift_check_test.cpp)
target_link_libraries(isa_vshift_check_test PRIVATE gta6rt_isa gta6rt_build_options)
