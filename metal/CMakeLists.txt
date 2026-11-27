cmake_minimum_required(VERSION 3.24)

# Standalone:  cmake -S port/metal -B port/build/metal-<arch> -G Ninja -DCMAKE_OSX_ARCHITECTURES=<arch>
# From port/:  add_subdirectory(metal)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(gta6rt_metal LANGUAGES C CXX OBJCXX)
  set(GTA6RT_METAL_STANDALONE ON)
  if(NOT APPLE)
    message(FATAL_ERROR "gta6rt_metal needs macOS")
  endif()
  list(LENGTH CMAKE_OSX_ARCHITECTURES _metal_n_arch)
  if(NOT _metal_n_arch EQUAL 1)
    message(FATAL_ERROR "set exactly one CMAKE_OSX_ARCHITECTURES (x86_64 or arm64) per build tree")
  endif()
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  set(CMAKE_OBJCXX_STANDARD 20)
  set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
  enable_testing()
else()
  enable_language(OBJCXX)
  set(GTA6RT_METAL_STANDALONE OFF)
endif()

option(GTA6RT_METAL_TESTS "Build the gta6rt_metal host tests" ${GTA6RT_METAL_STANDALONE})
set(GTA6RT_METAL_SPIRV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../spike/s2-make-game/work/shaders/spirv"
    CACHE PATH "leonida SPIR-V corpus for the Route A shader test (read only; nothing is copied into the source tree)")
set(GTA6RT_METAL_SPIRV_CROSS "/opt/homebrew/bin/spirv-cross" CACHE FILEPATH "spirv-cross used by tools/route_a_msl.py")

set(_metal_mm
  src/mtl_device.mm
  src/mtl_resource.mm
  src/mtl_pipeline.mm
  src/mtl_command.mm
  src/mtl_queue.mm
  src/mtl_window.mm
  src/mtl_formats.mm
  src/mtl_memory.mm
  src/mtl_present.mm)
add_library(gta6rt_metal STATIC ${_metal_mm} src/rect_list.cpp src/msl_rewrite.cpp)
set_source_files_properties(${_metal_mm} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_include_directories(gta6rt_metal PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(gta6rt_metal PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../hud/include")
target_compile_features(gta6rt_metal PUBLIC cxx_std_20)
target_compile_options(gta6rt_metal PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(gta6rt_metal PUBLIC
  "-framework Metal" "-framework QuartzCore" "-framework AppKit" "-framework Foundation")
if(TARGET gta6rt_build_options)
  target_link_libraries(gta6rt_metal PRIVATE gta6rt_build_options)
endif()

if(GTA6RT_METAL_TESTS)
  set(_metal_tests device present triangle rect_list game_shaders hazards compute draw2 mesh hostmem pipeline_cache present2 memory texture_view copy_fill)
  foreach(_t IN LISTS _metal_tests)
    add_executable(metal_test_${_t} tests/test_${_t}.cpp tests/test_util.cpp)
    target_link_libraries(metal_test_${_t} PRIVATE gta6rt_metal)
    target_compile_definitions(metal_test_${_t} PRIVATE
      GTA6RT_METAL_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders")
  endforeach()

  if(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
    target_link_options(metal_test_hostmem PRIVATE "LINKER:-pagezero_size,0x10000")
  endif()

  find_package(Python3 COMPONENTS Interpreter)
  set(_route_a_out "${CMAKE_CURRENT_BINARY_DIR}/route_a")
  if(Python3_Interpreter_FOUND AND EXISTS "${GTA6RT_METAL_SPIRV_DIR}" AND EXISTS "${GTA6RT_METAL_SPIRV_CROSS}")
    add_custom_command(
      OUTPUT "${_route_a_out}/manifest.txt"
      COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/tools/route_a_msl.py"
              --spirv-dir "${GTA6RT_METAL_SPIRV_DIR}" --spirv-cross "${GTA6RT_METAL_SPIRV_CROSS}"
              --out "${_route_a_out}" --metallib
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/route_a_msl.py"
      COMMENT "Route A: SPIRV-Cross MSL 3.0 for the game shader pairs"
      VERBATIM)
    add_custom_target(metal_route_a_msl ALL DEPENDS "${_route_a_out}/manifest.txt")
    add_dependencies(metal_test_game_shaders metal_route_a_msl)
    add_dependencies(metal_test_compute metal_route_a_msl)
    add_dependencies(metal_test_mesh metal_route_a_msl)
    add_dependencies(metal_test_pipeline_cache metal_route_a_msl)
  endif()

  add_test(NAME metal_device COMMAND metal_test_device)
  add_test(NAME metal_present COMMAND metal_test_present)
  add_test(NAME metal_triangle COMMAND metal_test_triangle)
  add_test(NAME metal_rect_list COMMAND metal_test_rect_list)
  add_test(NAME metal_game_shaders COMMAND metal_test_game_shaders "${_route_a_out}/manifest.txt")
  add_test(NAME metal_hazards COMMAND metal_test_hazards)
  add_test(NAME metal_hazards_validation COMMAND metal_test_hazards --iterations 40 --heavy 400)
  add_test(NAME metal_hazards_table_sets COMMAND metal_test_hazards --only bindless,wait_split --heavy 400)
  add_test(NAME metal_hazards_epoch COMMAND metal_test_hazards --only bindless)
  add_test(NAME metal_hazards_residency_check COMMAND metal_test_hazards --iterations 40 --heavy 400)
  add_test(NAME metal_hazards_residency_negative COMMAND metal_test_hazards --only bindless,heap_base --iterations 10 --heavy 400)
  add_test(NAME metal_hazards_validation_fault COMMAND metal_test_hazards --only validation_fault)
  add_test(NAME metal_hazards_gpu_error COMMAND metal_test_hazards --only gpu_error)
  add_test(NAME metal_hazards_gpu_error_no_repair COMMAND metal_test_hazards --only gpu_error)
  add_test(NAME metal_hazards_own_error COMMAND metal_test_hazards --only same_list --iterations 12 --heavy 400 --inject 5)
  add_test(NAME metal_hazards_victim_retry COMMAND metal_test_hazards --only same_list --iterations 12 --heavy 400 --inject 5 --inject-victim)
  add_test(NAME metal_compute COMMAND metal_test_compute "${_route_a_out}/manifest.txt")
  add_test(NAME metal_draw2 COMMAND metal_test_draw2)
  add_test(NAME metal_mesh COMMAND metal_test_mesh "${_route_a_out}/manifest.txt")
  add_test(NAME metal_hostmem COMMAND metal_test_hostmem)
  add_test(NAME metal_present2 COMMAND metal_test_present2)
  add_test(NAME metal_memory COMMAND metal_test_memory)
  add_test(NAME metal_texture_view COMMAND metal_test_texture_view)
  add_test(NAME metal_copy_fill COMMAND metal_test_copy_fill)
  add_test(NAME metal_hostmem_validation COMMAND metal_test_hostmem)
  set(_pcache "${CMAKE_CURRENT_BINARY_DIR}/pipeline-cache")
  add_test(NAME metal_pipeline_cache_cold COMMAND metal_test_pipeline_cache --phase cold --dir "${_pcache}" "${_route_a_out}/manifest.txt")
  add_test(NAME metal_pipeline_cache_warm COMMAND metal_test_pipeline_cache --phase warm --dir "${_pcache}" "${_route_a_out}/manifest.txt")
  add_test(NAME metal_pipeline_cache_warm2 COMMAND metal_test_pipeline_cache --phase warm2 --dir "${_pcache}" "${_route_a_out}/manifest.txt")
  add_test(NAME metal_pipeline_async COMMAND metal_test_pipeline_cache --phase async "${_route_a_out}/manifest.txt")
  set_tests_properties(metal_pipeline_cache_cold PROPERTIES FIXTURES_SETUP metal_pcache)
  set_tests_properties(metal_pipeline_cache_warm PROPERTIES FIXTURES_REQUIRED metal_pcache FIXTURES_SETUP metal_pcache2)
  set_tests_properties(metal_pipeline_cache_warm2 PROPERTIES FIXTURES_REQUIRED metal_pcache2)
  set_tests_properties(metal_pipeline_cache_cold metal_pipeline_cache_warm metal_pipeline_cache_warm2 metal_pipeline_async PROPERTIES
                       ENVIRONMENT "MTL_SHADER_CACHE_SIZE=0" TIMEOUT 300 SKIP_RETURN_CODE 77)
  set_tests_properties(metal_device metal_present metal_triangle metal_rect_list metal_game_shaders metal_hazards
                       metal_hazards_validation metal_hazards_table_sets metal_hazards_epoch metal_hazards_residency_check
                       metal_hazards_residency_negative metal_hazards_validation_fault metal_hazards_gpu_error
                       metal_hazards_gpu_error_no_repair metal_hazards_own_error metal_hazards_victim_retry metal_compute metal_draw2 metal_mesh metal_hostmem metal_hostmem_validation
                       metal_present2 metal_memory
                       PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
  set_tests_properties(metal_hazards_validation PROPERTIES
                       ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1")
  set_tests_properties(metal_hostmem_validation PROPERTIES
                       ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1")
  set_tests_properties(metal_hazards_table_sets PROPERTIES
                       ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1;GTA6RT_METAL_NO_DEVICE_RESIDENCY=1")
  # Resource-usage validation reports every access to a non-resident resource on stderr (the negative control shows it).
  set(_metal_residency_env "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1;MTL_SHADER_VALIDATION_RESOURCE_USAGE=1;MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1")
  set_tests_properties(metal_hazards_residency_check PROPERTIES
                       ENVIRONMENT "${_metal_residency_env}" FAIL_REGULAR_EXPRESSION "Invalid Metal usage")
  set_tests_properties(metal_hazards_residency_negative PROPERTIES
                       ENVIRONMENT "${_metal_residency_env};GTA6RT_METAL_NO_RESIDENCY=1"
                       PASS_REGULAR_EXPRESSION "Texture is not resident")
  set_tests_properties(metal_hazards_validation_fault PROPERTIES
                       ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1;MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1")
  # A real command-buffer failure (inject_command_buffer_failure). The no-repair run is the negative control; an
  # injected own error must fail the hazard case, an injected victim must be repeated.
  set_tests_properties(metal_hazards_gpu_error_no_repair PROPERTIES ENVIRONMENT "GTA6RT_METAL_NO_ERROR_REPAIR=1")
  set_tests_properties(metal_hazards_own_error PROPERTIES
                       PASS_REGULAR_EXPRESSION "1 of 12 iterations read a wrong target \\(1 reads failed; 0 repeated")
  set_tests_properties(metal_hazards_victim_retry PROPERTIES
                       PASS_REGULAR_EXPRESSION "0 of 12 iterations read a wrong target \\(1 reads failed; 1 repeated")
endif()
