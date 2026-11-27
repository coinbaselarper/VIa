# Standalone build of the window layer's tests (the host's own CMakeLists is the integrator's):
#   cmake -S port/host/src/win/tests -B port/build/win-x86_64 -G Ninja -DCMAKE_OSX_ARCHITECTURES=x86_64
#   ninja -C port/build/win-x86_64 -j3
cmake_minimum_required(VERSION 3.24)
project(host_win_tests LANGUAGES C CXX OBJCXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
get_filename_component(PORT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(WIN "${CMAKE_CURRENT_LIST_DIR}/..")

add_executable(win_core_test win_core_test.cpp "${WIN}/win_core.cpp")

add_library(win_layer STATIC
  "${WIN}/../window_shim.cpp" "${WIN}/win_core.cpp" "${WIN}/appkit_window.mm"
  "${PORT}/d3d12_vulkan/platform/metal_window.mm" "${PORT}/hud/src/hud.mm" "${PORT}/hud/src/hud_model.cpp" stubs.cpp)
set_source_files_properties("${WIN}/appkit_window.mm" "${PORT}/d3d12_vulkan/platform/metal_window.mm" "${PORT}/hud/src/hud.mm" PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_include_directories(win_layer PRIVATE "${PORT}/hud/include" "${PORT}/hud/src")
target_include_directories(win_layer PUBLIC "${WIN}" "${WIN}/.." "${CMAKE_CURRENT_LIST_DIR}" "${PORT}/d3d12_vulkan/platform" "${PORT}/include" "${PORT}/shims_core/src")
target_include_directories(win_layer SYSTEM PUBLIC "${PORT}/compat/include")
target_compile_definitions(win_layer PUBLIC NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_link_libraries(win_layer PUBLIC "-framework AppKit" "-framework QuartzCore" "-framework Metal" "-framework Foundation")

add_executable(win_shim_test win_shim_test.cpp)
target_link_libraries(win_shim_test PRIVATE win_layer "-Wl,-force_load,$<TARGET_FILE:win_layer>")
add_executable(win_gui_test win_gui_test.cpp gui_helpers.mm)
set_source_files_properties(gui_helpers.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_link_libraries(win_gui_test PRIVATE win_layer "-Wl,-force_load,$<TARGET_FILE:win_layer>")

enable_testing()
add_test(NAME win_core COMMAND win_core_test)
add_test(NAME win_shim COMMAND win_shim_test)
