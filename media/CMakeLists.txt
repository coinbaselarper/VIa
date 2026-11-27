# gta6rt_media: XAudio2 2.9 (CoreAudio / silent output) and the Media Foundation subset the title imports.
# Works as a module of port/CMakeLists.txt (add "media" to GTA6RT_ALL_MODULES) and under media/dev (own build trees).
get_filename_component(GTA6RT_MEDIA_PORT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

add_library(gta6rt_media STATIC
  src/media_common.cpp
  src/audio_output_silent.cpp
  src/audio_output_coreaudio.cpp
  src/xaudio2_engine.cpp
  src/xaudio2_com.cpp
  src/mf.cpp
  src/media_register.cpp)
target_include_directories(gta6rt_media PRIVATE src "${GTA6RT_MEDIA_PORT_ROOT}/shims_core/src")
target_link_libraries(gta6rt_media PUBLIC gta6rt_interfaces PRIVATE gta6rt_build_options gta6rt_os gta6rt_isa gta6rt_wincompat)
target_compile_features(gta6rt_media PUBLIC cxx_std_20)
target_compile_options(gta6rt_media PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(gta6rt_media PUBLIC "-framework AudioToolbox" "-framework AudioUnit" "-framework CoreAudio"
  "-framework CoreFoundation")

option(GTA6RT_MEDIA_TESTS "Build the gta6rt_media tests" ON)
if(GTA6RT_MEDIA_TESTS)
  # Unit tests drive the guest-facing COM objects directly from the host (a fake client), with a direct invoker for
  # callbacks. They link gta6rt_os/gta6rt_isa but not shims_core (media_register.o is not pulled in).
  foreach(_t xaudio2_test mf_test xaudio2_timing_test)
    add_executable(media_${_t} tests/${_t}.cpp)
    target_include_directories(media_${_t} PRIVATE src)
    target_link_libraries(media_${_t} PRIVATE gta6rt_media gta6rt_build_options gta6rt_os gta6rt_isa)
  endforeach()

  if(TARGET gta6rt_shims_core)
    add_executable(media_link_check tests/link_check.cpp)
    target_include_directories(media_link_check PRIVATE src)
    target_link_libraries(media_link_check PRIVATE gta6rt_media gta6rt_shims_core gta6rt_isa gta6rt_os gta6rt_portable
      gta6rt_wincompat gta6rt_dxguids gta6rt_build_options)
    target_link_options(media_link_check PRIVATE "LINKER:-no_warn_duplicate_libraries"
      "LINKER:-force_load,$<TARGET_FILE:gta6rt_shims_core>")
  endif()
endif()

# The XMA patch proposal for shims_gdk (patches/acphal-xma-fixes.patch), checked against a patched copy of
# shims_gdk/src/acphal.cpp with a stand-in XMA2 decoder. shims_gdk itself is never modified.
option(GTA6RT_MEDIA_XMA_PATCH_TEST "Build the acphal XMA patch test (needs the shims_gdk targets)" ON)
if(GTA6RT_MEDIA_XMA_PATCH_TEST AND TARGET gta6rt_shims_gdk AND TARGET gdk_test_support)
  set(_gdk "${GTA6RT_MEDIA_PORT_ROOT}/shims_gdk")
  set(_patched "${CMAKE_CURRENT_BINARY_DIR}/xma_patched/acphal.cpp")
  set(_unpatched "${CMAKE_CURRENT_BINARY_DIR}/xma_unpatched/acphal.cpp")
  add_custom_command(OUTPUT "${_patched}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/xma_patched"
    COMMAND patch -s -o "${_patched}" "${_gdk}/src/acphal.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/patches/acphal-xma-fixes.patch"
    DEPENDS "${_gdk}/src/acphal.cpp" patches/acphal-xma-fixes.patch VERBATIM)
  add_custom_command(OUTPUT "${_unpatched}"
    COMMAND ${CMAKE_COMMAND} -E copy "${_gdk}/src/acphal.cpp" "${_unpatched}"
    DEPENDS "${_gdk}/src/acphal.cpp" VERBATIM)
  set_source_files_properties(tests/xma/xma_fix_test.cpp PROPERTIES
    COMPILE_OPTIONS "-include;${_gdk}/tests/support/test_prelude.h")
  foreach(_v patched unpatched)
    add_library(media_acphal_${_v} STATIC "${_${_v}}" tests/xma/fake_xma2_decoder.cpp)
    target_include_directories(media_acphal_${_v} PRIVATE tests/xma "${_gdk}/shared" "${_gdk}/support" "${_gdk}/src")
    target_compile_definitions(media_acphal_${_v} PRIVATE LEONIDA_HAVE_XMA2_DECODER=1)
    target_compile_options(media_acphal_${_v} PRIVATE -w)
    target_link_libraries(media_acphal_${_v} PRIVATE gta6rt_wincompat gta6rt_os)
    add_executable(media_xma_fix_test_${_v} tests/xma/xma_fix_test.cpp "${_gdk}/src/gdk_register.cpp"
      "${_gdk}/src/silent_audio.cpp" "${_gdk}/src/gdk_platform_darwin.cpp")
    target_include_directories(media_xma_fix_test_${_v} PRIVATE "${_gdk}/shared" "${_gdk}/support" "${_gdk}/src")
    target_compile_options(media_xma_fix_test_${_v} PRIVATE -Wno-deprecated-declarations
      -Wno-inconsistent-missing-override)
    target_link_libraries(media_xma_fix_test_${_v} PRIVATE gdk_test_support gta6rt_os gta6rt_wincompat
      "-framework IOKit" "-framework CoreFoundation")
    target_link_options(media_xma_fix_test_${_v} PRIVATE "LINKER:-force_load,$<TARGET_FILE:media_acphal_${_v}>")
    add_dependencies(media_xma_fix_test_${_v} media_acphal_${_v})
  endforeach()
  target_compile_definitions(media_xma_fix_test_patched PRIVATE XMA_FIX_EXPECT_PATCHED=1)
endif()
