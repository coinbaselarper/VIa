# leonida runtime files that compile on macOS unchanged (spike/s1-host-build-scout). Copies; see ORIGIN.md.

set(bcpack_tables "${CMAKE_CURRENT_BINARY_DIR}/generated/bcpack_tables.inc")
add_custom_command(OUTPUT "${bcpack_tables}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
  COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
          "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_bcpack_runtime_tables.py" "${bcpack_tables}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_bcpack_runtime_tables.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/bcpack.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/bcpack_tables.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/bcpack_tables_bptc.py"
  VERBATIM)

add_library(gta6rt_portable STATIC
  src/root_signature.cpp
  src/bcpack.cpp
  src/vulkan_aftermath.cpp
  "${bcpack_tables}")
target_include_directories(gta6rt_portable
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
  PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(gta6rt_portable PUBLIC gta6rt_build_options gta6rt_vulkan_headers)

add_library(gta6rt_d3d12_portable STATIC
  shims/d3d12_xs_pso_stream.cpp
  shims/d3d12_xs_rootsig.cpp)
target_include_directories(gta6rt_d3d12_portable PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/shims")
target_link_libraries(gta6rt_d3d12_portable PUBLIC gta6rt_portable gta6rt_dxheaders)

add_executable(portable_link_check tests/portable_link_check.cpp)
target_link_libraries(portable_link_check PRIVATE gta6rt_d3d12_portable gta6rt_dxguids)
