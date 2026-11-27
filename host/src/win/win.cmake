# Sources and libraries of the host's window layer (host/src/window_shim.cpp + win/). The integrator includes this
# from host/CMakeLists.txt after add_executable(gta6rt ...):
#   include(src/win/win.cmake)
# It adds the win/ sources to the gta6rt target and the frameworks they use. window_shim.cpp itself is already in
# the target's source list. appkit_window.mm is compiled as Objective-C++ through the C++ rule (-x objective-c++), so
# the directory needs no enable_language(OBJCXX).
set(_gta6rt_win_dir "${CMAKE_CURRENT_LIST_DIR}")
target_sources(gta6rt PRIVATE
  "${_gta6rt_win_dir}/win_core.cpp"
  "${_gta6rt_win_dir}/appkit_window.mm")
set_source_files_properties("${_gta6rt_win_dir}/appkit_window.mm" TARGET_DIRECTORY gta6rt PROPERTIES
  LANGUAGE CXX COMPILE_OPTIONS "-x;objective-c++;-fobjc-arc")
target_link_libraries(gta6rt PRIVATE "-framework AppKit" "-framework QuartzCore" "-framework Foundation")
