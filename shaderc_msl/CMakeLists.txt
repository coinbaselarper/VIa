cmake_minimum_required(VERSION 3.24)
project(gta6rt_shaderc_msl LANGUAGES C CXX OBJCXX)

if(NOT APPLE)
  message(FATAL_ERROR "gta6rt_shaderc_msl needs macOS (Metal)")
endif()

set(_shaderc_standalone OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_shaderc_standalone ON)
endif()
option(GTA6RT_SHADERC_MSL_TESTS "Build the shaderc_msl tests" ${_shaderc_standalone})

get_filename_component(_shaderc_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(GTA6RT_SHADERC_MSL_SPIRV_DIR "${_shaderc_root}/spike/s2-make-game/work/shaders/spirv" CACHE PATH
    "translated SPIR-V corpus with index.json (read only)")
set(GTA6RT_SHADERC_MSL_OUT "${_shaderc_root}/port/build/shaderc_msl" CACHE PATH
    "where the corpus tools write (content-addressed metallibs, runs, reports)")
find_program(GTA6RT_SHADERC_MSL_PYTHON NAMES python3.14 python3.13 python3.12 python3.11 python3
             HINTS /opt/homebrew/bin)

add_library(gta6rt_shaderc_msl STATIC src/catalog.cpp)
target_include_directories(gta6rt_shaderc_msl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(gta6rt_shaderc_msl PUBLIC cxx_std_20)

add_executable(shaderc_msl_probe tools/msl_probe.mm)
target_compile_features(shaderc_msl_probe PRIVATE cxx_std_20)
target_compile_options(shaderc_msl_probe PRIVATE -fobjc-arc)
target_link_libraries(shaderc_msl_probe PRIVATE "-framework Metal" "-framework Foundation")

add_executable(shaderc_msl_mesh_run tools/mesh_run.mm)
target_compile_features(shaderc_msl_mesh_run PRIVATE cxx_std_20)
target_compile_options(shaderc_msl_mesh_run PRIVATE -fobjc-arc)
target_link_libraries(shaderc_msl_mesh_run PRIVATE "-framework Metal" "-framework Foundation")

if(TARGET gta6rt_metal)
  add_executable(shaderc_msl_warmup tools/warmup/warmup_build.cpp)
  target_link_libraries(shaderc_msl_warmup PRIVATE gta6rt_metal gta6rt_shaderc_msl)
endif()

set(_tools ${CMAKE_CURRENT_SOURCE_DIR}/tools)
set(_py ${GTA6RT_SHADERC_MSL_PYTHON} -I)
set(_corpus --spirv-dir ${GTA6RT_SHADERC_MSL_SPIRV_DIR})
get_filename_component(_s2 "${GTA6RT_SHADERC_MSL_SPIRV_DIR}/../../.." ABSOLUTE)
add_custom_target(shaderc_msl_pairs
  COMMAND ${_py} ${_tools}/pipelines.py --rpf ${_s2}/game-full/shaders.rpf --dxbc-dir ${_s2}/work/shaders/dxbc
          --out ${GTA6RT_SHADERC_MSL_OUT}/pipelines
  COMMENT "pipeline pairing from the title's pipeline library" VERBATIM USES_TERMINAL)
add_custom_target(shaderc_msl_mesh_fit
  COMMAND ${_py} ${_tools}/mesh_fit.py ${_corpus} --pairs ${GTA6RT_SHADERC_MSL_OUT}/pipelines/pairs.json
          --out ${GTA6RT_SHADERC_MSL_OUT}/mesh --jobs 3
  COMMENT "mesh output-limit steps" VERBATIM USES_TERMINAL)
add_custom_target(shaderc_msl_gs_census
  COMMAND ${_py} ${_tools}/gs_census.py ${_corpus} --pairs ${GTA6RT_SHADERC_MSL_OUT}/pipelines/pairs.json
          --exe ${_s2}/game-full/game_scarlett_master.exe --out ${GTA6RT_SHADERC_MSL_OUT}/gs
  COMMENT "geometry-shader census" VERBATIM USES_TERMINAL)
add_custom_target(shaderc_msl_verify_bindings
  COMMAND ${_py} ${_tools}/verify_bindings.py ${_corpus} --build ${GTA6RT_SHADERC_MSL_OUT} --jobs 4
  COMMENT "binding plan check by Metal reflection" VERBATIM USES_TERMINAL)
add_custom_target(shaderc_msl_cs_check
  COMMAND ${_py} ${_tools}/cs_check.py ${_corpus} --build ${GTA6RT_SHADERC_MSL_OUT} --skip-done
  COMMENT "compute shaders at Metal limits: threadgroup memory, fp64, huge" VERBATIM USES_TERMINAL)
add_custom_target(shaderc_msl_mesh_pso
  COMMAND ${_py} ${_tools}/mesh_pso.py ${_corpus} --build ${GTA6RT_SHADERC_MSL_OUT} --jobs 1
  COMMENT "mesh steps measured with real pipelines" VERBATIM USES_TERMINAL)
add_custom_target(shaderc_msl_mesh_route
  COMMAND ${_py} ${_tools}/mesh_route.py build --build ${GTA6RT_SHADERC_MSL_OUT} --jobs 2
  COMMAND ${_py} ${_tools}/mesh_route.py decide --build ${GTA6RT_SHADERC_MSL_OUT}
  COMMENT "mesh route decision: split variants built from the sweep, mesh_route.json" VERBATIM USES_TERMINAL)
add_custom_target(shaderc_msl_final_counts
  COMMAND ${_py} ${_tools}/final_counts.py --build ${GTA6RT_SHADERC_MSL_OUT} ${_corpus}
  COMMENT "per-stage counts" VERBATIM USES_TERMINAL)
add_custom_target(shaderc_msl_warmup_list
  COMMAND ${_py} ${_tools}/warmup_list.py --logs "${_shaderc_root}/port/run/logs/r4pso-*.log"
          --manifest ${GTA6RT_SHADERC_MSL_OUT}/runs/main/manifest.tsv --out ${CMAKE_CURRENT_SOURCE_DIR}/warmup/boot_pipelines.json
  COMMENT "boot-path pipeline list from the census logs" VERBATIM USES_TERMINAL)
add_custom_target(shaderc_msl_batch
  COMMAND ${_py} ${_tools}/batch.py ${_corpus} --build ${GTA6RT_SHADERC_MSL_OUT} --jobs 4
  COMMENT "corpus batch compile (resumable)" VERBATIM USES_TERMINAL)

if(GTA6RT_SHADERC_MSL_TESTS)
  enable_testing()
  foreach(t soft_f64_test vs_lane_test vs_lane_identity_test catalog_test)
    add_executable(shaderc_msl_${t} tests/${t}.mm)
    target_compile_features(shaderc_msl_${t} PRIVATE cxx_std_20)
    target_compile_options(shaderc_msl_${t} PRIVATE -fobjc-arc)
    target_link_libraries(shaderc_msl_${t} PRIVATE gta6rt_shaderc_msl "-framework Metal" "-framework Foundation")
  endforeach()
  add_test(NAME shaderc_msl_soft_f64 COMMAND shaderc_msl_soft_f64_test ${_tools}/soft_f64.metal)
  add_test(NAME shaderc_msl_vs_lane_identity COMMAND shaderc_msl_vs_lane_identity_test)

  set(_vs_lane_dir ${CMAKE_CURRENT_BINARY_DIR}/vs_lane)
  if(EXISTS ${GTA6RT_SHADERC_MSL_SPIRV_DIR}/index.json AND GTA6RT_SHADERC_MSL_PYTHON)
    add_custom_command(OUTPUT ${_vs_lane_dir}/manifest.txt
      COMMAND ${_py} ${_tools}/vs_lane_gen.py ${_corpus} --out ${_vs_lane_dir} --limit 40
      DEPENDS ${_tools}/vs_lane_gen.py ${_tools}/shaderc_msl.py ${_tools}/spv.py
      COMMENT "vertex lane-id test inputs (40 corpus vertex shaders, both lowerings)" VERBATIM)
    add_custom_target(shaderc_msl_vs_lane_inputs ALL DEPENDS ${_vs_lane_dir}/manifest.txt)
  endif()
  add_test(NAME shaderc_msl_vs_lane COMMAND shaderc_msl_vs_lane_test ${_vs_lane_dir}/manifest.txt --expect-equal=40)
  add_test(NAME shaderc_msl_catalog COMMAND shaderc_msl_catalog_test ${GTA6RT_SHADERC_MSL_OUT}/runs/main/manifest.tsv)
  set_tests_properties(shaderc_msl_vs_lane shaderc_msl_catalog shaderc_msl_vs_lane_identity PROPERTIES SKIP_RETURN_CODE 77)
  set_tests_properties(shaderc_msl_soft_f64 shaderc_msl_vs_lane shaderc_msl_catalog shaderc_msl_vs_lane_identity PROPERTIES
    ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1")
endif()
