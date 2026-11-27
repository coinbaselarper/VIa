# S1.5b GDK shims: xgameruntime*, dstorage, acphal, xapu_chad, silent_audio, gameinput (mac), xtaskqueue.
# Shims keep leonida's registration tables (SHIM_TABLE); gdk_register.cpp forwards them to gta6rt_shims_register_dll
# (gta6rt_shims_gdk_register). Link the archive with -force_load: the tables register from static constructors.
# The library has undefined references that shims_core and os resolve in the final link (the final link supplies them);
# it does not link them itself so the tests can supply stand-ins. shared/ holds copies of leonida headers that
# shims_core owns (shims.h, startup_trace.h, console_identity.h, file_system.h, xmem.h).

enable_language(OBJCXX)

add_library(gta6rt_xtaskqueue STATIC
  xtaskqueue/Source/Task/TaskQueue.cpp
  xtaskqueue/Source/Task/AsyncLib.cpp
  xtaskqueue/Source/Task/ThreadPool_posix.cpp
  xtaskqueue/Source/Task/WaitTimer_posix.cpp)
target_include_directories(gta6rt_xtaskqueue
  PRIVATE xtaskqueue xtaskqueue/Source/Task xtaskqueue/Source/Common support
  PUBLIC xtaskqueue/Include)
target_compile_options(gta6rt_xtaskqueue PRIVATE -w)
target_link_libraries(gta6rt_xtaskqueue PUBLIC gta6rt_wincompat PRIVATE gta6rt_os)

set(GDK_SHIM_SOURCES
  src/xgameruntime.cpp
  src/xgameruntime_user.cpp
  src/xgameruntime_gamesave.cpp
  src/xgameruntime_store.cpp
  src/xgameruntime_offline.cpp
  src/xapu_chad.cpp
  src/gameinput_mac.cpp
  src/gameinput_gc.mm
  src/acphal.cpp
  src/dstorage.cpp
  src/gdk_title_memory.cpp
  src/silent_audio.cpp
  src/gdk_register.cpp
  src/gdk_platform_darwin.cpp)
add_library(gta6rt_shims_gdk STATIC ${GDK_SHIM_SOURCES})
target_include_directories(gta6rt_shims_gdk PRIVATE shared support src ../portable/src)
find_package(ZLIB REQUIRED)
target_link_libraries(gta6rt_shims_gdk PRIVATE gta6rt_portable gta6rt_dxheaders ZLIB::ZLIB)
target_link_libraries(gta6rt_shims_gdk PUBLIC gta6rt_interfaces gta6rt_xtaskqueue PRIVATE gta6rt_wincompat gta6rt_os)
target_link_libraries(gta6rt_shims_gdk PRIVATE "-framework IOKit" "-framework CoreFoundation" "-framework Foundation"
  "-framework AppKit" "-framework GameController" "-framework CoreHaptics")
set_source_files_properties(src/gameinput_gc.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
if(TARGET gta6rt_audio_out)
  target_link_libraries(gta6rt_shims_gdk PRIVATE gta6rt_audio_out)
  target_compile_definitions(gta6rt_shims_gdk PRIVATE GTA6RT_HAVE_AUDIO_OUT=1)
endif()
if(TARGET gta6rt_xma)
  target_link_libraries(gta6rt_shims_gdk PRIVATE gta6rt_xma)
endif()

option(GTA6RT_GDK_TESTS "Build the shims_gdk tests" ON)
option(GTA6RT_GDK_TEST_WIN32_EMUL "Link the tests against tests/support/win32_emul.cpp instead of gta6rt_os Win32 surface" OFF)
if(GTA6RT_GDK_TESTS)
  enable_testing()
  add_library(gdk_test_support STATIC
    tests/support/registry_min.cpp
    tests/support/fs_stub.cpp
    tests/support/com_stub.cpp
    tests/support/xmem_fake.cpp)
  target_include_directories(gdk_test_support PUBLIC shared support src)
  target_link_libraries(gdk_test_support PUBLIC gta6rt_wincompat gta6rt_interfaces)
  if(GTA6RT_GDK_TEST_WIN32_EMUL)
    target_sources(gdk_test_support PRIVATE tests/support/win32_emul.cpp)
  endif()

  function(gdk_add_test name)
    cmake_parse_arguments(T "" "" "SOURCES;LIBS;INCLUDES;DEFS" ${ARGN})
    add_executable(${name} ${T_SOURCES})
    target_include_directories(${name} PRIVATE ${T_INCLUDES})
    target_compile_definitions(${name} PRIVATE ${T_DEFS})
    target_compile_options(${name} PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/test_prelude.h
      -Wno-inconsistent-missing-override -Wno-deprecated-declarations)
    target_link_libraries(${name} PRIVATE ${T_LIBS} gdk_test_support gta6rt_os)
  endfunction()

  set(XTQ_INCLUDES xtaskqueue xtaskqueue/Source/Task xtaskqueue/Source/Common support)
  gdk_add_test(api_ref_state_test SOURCES tests/api_ref_state_test.cpp INCLUDES ${XTQ_INCLUDES})
  gdk_add_test(api_global_refs_test SOURCES tests/api_global_refs_test.cpp LIBS gta6rt_xtaskqueue)
  foreach(_t context_pinning context_rundown context_reentrant context_concurrent_detach context_attach_detach_stress)
    gdk_add_test(${_t}_test SOURCES tests/${_t}_test.cpp INCLUDES ${XTQ_INCLUDES} LIBS gta6rt_xtaskqueue)
  endforeach()
  gdk_add_test(wait_registration_test SOURCES tests/wait_registration_test.cpp INCLUDES ${XTQ_INCLUDES} LIBS gta6rt_xtaskqueue)

  set(GDK_FORCE_LOAD "-Wl,-force_load,$<TARGET_FILE:gta6rt_shims_gdk>")
  foreach(_t xgameruntime xgameruntime_user xgameruntime_gamesave xgameruntime_store xgameruntime_offline)
    gdk_add_test(${_t}_test SOURCES tests/${_t}_test.cpp LIBS gta6rt_shims_gdk ${GDK_FORCE_LOAD})
  endforeach()

  set(GDK_VECTORS "${CMAKE_CURRENT_BINARY_DIR}/dstorage_vectors.inc")
  add_custom_command(OUTPUT "${GDK_VECTORS}"
    COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/dstorage_vectors.py" "${GDK_VECTORS}"
    DEPENDS tests/vectors/dstorage_vectors.py tests/vectors/test_bcpack.py
            ../portable/tools/bcpack.py ../portable/tools/bcpack_tables.py ../portable/tools/bcpack_tables_bptc.py
    VERBATIM)
  gdk_add_test(dstorage_test SOURCES tests/dstorage_test.cpp "${GDK_VECTORS}"
    INCLUDES "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_BINARY_DIR}/portable/generated" LIBS gta6rt_shims_gdk gta6rt_portable gta6rt_dxheaders gta6rt_dxguids ${GDK_FORCE_LOAD})
  gdk_add_test(dstorage_native_fd_test SOURCES tests/dstorage_test.cpp tests/dstorage_native_fd_row.cpp "${GDK_VECTORS}"
    INCLUDES "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_BINARY_DIR}/portable/generated" LIBS gta6rt_shims_gdk gta6rt_portable gta6rt_dxheaders gta6rt_dxguids ${GDK_FORCE_LOAD})
  gdk_add_test(dstorage_pool_stress_test SOURCES tests/dstorage_pool_stress_test.cpp
    LIBS gta6rt_shims_gdk gta6rt_portable gta6rt_dxheaders gta6rt_dxguids ZLIB::ZLIB ${GDK_FORCE_LOAD})
  gdk_add_test(dstorage_pool_stress_native_fd_test SOURCES tests/dstorage_pool_stress_test.cpp tests/dstorage_native_fd_row.cpp
    LIBS gta6rt_shims_gdk gta6rt_portable gta6rt_dxheaders gta6rt_dxguids ZLIB::ZLIB ${GDK_FORCE_LOAD})

  foreach(_t acphal xapu_chad silent_audio gameinput_mac)
    gdk_add_test(${_t}_test SOURCES tests/${_t}_test.cpp LIBS gta6rt_shims_gdk ${GDK_FORCE_LOAD})
  endforeach()
  gdk_add_test(gameinput_pad_test SOURCES tests/gameinput_pad_test.cpp INCLUDES src)
  if(TARGET gta6rt_audio_out)
    gdk_add_test(audio_out_spatial_test SOURCES tests/audio_out_spatial_test.cpp LIBS gta6rt_shims_gdk gta6rt_audio_out ${GDK_FORCE_LOAD})
    add_test(NAME gdk_audio_out_spatial COMMAND audio_out_spatial_test)
    set_tests_properties(gdk_audio_out_spatial PROPERTIES TIMEOUT 60)
    gdk_add_test(xapu_opus_test SOURCES tests/xapu_opus_test.cpp LIBS gta6rt_shims_gdk gta6rt_audio_out ${GDK_FORCE_LOAD})
    add_test(NAME gdk_xapu_opus COMMAND xapu_opus_test)
    set_tests_properties(gdk_xapu_opus PROPERTIES TIMEOUT 60)
  endif()
  if(TARGET gta6rt_xma)
    gdk_add_test(acphal_xma_title_test SOURCES tests/acphal_xma_title_test.cpp INCLUDES "${GTA6RT_ROOT}/xma/tests"
      LIBS gta6rt_shims_gdk gta6rt_xma ${GDK_FORCE_LOAD})
    add_test(NAME gdk_acphal_xma_title COMMAND acphal_xma_title_test)
    add_test(NAME gdk_acphal_xma_title_host COMMAND acphal_xma_title_test)
    set_tests_properties(gdk_acphal_xma_title PROPERTIES TIMEOUT 120 ENVIRONMENT "GTA6RT_AUDIO=off")
    set_tests_properties(gdk_acphal_xma_title_host PROPERTIES TIMEOUT 120 ENVIRONMENT "GTA6RT_AUDIO=null")
  endif()
  gdk_add_test(gameinput_probe SOURCES tests/gameinput_probe.cpp LIBS gta6rt_shims_gdk ${GDK_FORCE_LOAD})

  set(_gdk_plain_tests api_ref_state api_global_refs context_pinning context_rundown context_reentrant
      context_concurrent_detach context_attach_detach_stress xgameruntime xgameruntime_user xgameruntime_gamesave
      xgameruntime_store xgameruntime_offline dstorage dstorage_native_fd acphal xapu_chad silent_audio gameinput_mac gameinput_pad)
  foreach(_t ${_gdk_plain_tests})
    add_test(NAME gdk_${_t} COMMAND ${_t}_test)
    set_tests_properties(gdk_${_t} PROPERTIES TIMEOUT 240)
  endforeach()
  add_test(NAME gdk_dstorage_pool_stress COMMAND dstorage_pool_stress_test 600 2)
  add_test(NAME gdk_dstorage_pool_stress_native_fd COMMAND dstorage_pool_stress_native_fd_test 600 2)
  set_tests_properties(gdk_dstorage_pool_stress gdk_dstorage_pool_stress_native_fd PROPERTIES TIMEOUT 240)
  add_test(NAME gdk_acphal_noncached_only COMMAND acphal_test --noncached-only)
  add_test(NAME gdk_gameinput_mac_script COMMAND gameinput_mac_test --input-script)
  add_test(NAME gdk_gameinput_mac_synthetic COMMAND gameinput_mac_test --synthetic)
  add_test(NAME gdk_gameinput_mac_census COMMAND gameinput_mac_test --census)
  add_test(NAME gdk_gameinput_mac_sticky COMMAND gameinput_mac_test --sticky)
  add_test(NAME gdk_gameinput_mac_late COMMAND gameinput_mac_test --late)
  add_test(NAME gdk_gameinput_mac_alongside COMMAND gameinput_mac_test --alongside)
  add_test(NAME gdk_gameinput_mac_axes COMMAND gameinput_mac_test --axes)
  foreach(_case unregister undispatched terminate parked self presignaled monitor close composite snapshot cross
                concurrent held_snapshot admission cancel_unregister reentrant_admission
                reentrant_vs_external_unregister multi_dispatcher_terminate multi_dispatcher_unregister_replace)
    add_test(NAME gdk_wait_registration_${_case} COMMAND wait_registration_test ${_case})
    set_tests_properties(gdk_wait_registration_${_case} PROPERTIES TIMEOUT 30)
  endforeach()
endif()

if(GTA6RT_GDK_TESTS AND TARGET gta6rt_shims_core AND TARGET gta6rt_isa)
  add_executable(gdk_link_check tests/link_check.cpp)
  target_include_directories(gdk_link_check PRIVATE shared src)
  target_link_libraries(gdk_link_check PRIVATE gta6rt_shims_core gta6rt_shims_gdk gta6rt_isa gta6rt_os gta6rt_portable
    gta6rt_wincompat gta6rt_dxguids)
  target_link_options(gdk_link_check PRIVATE "LINKER:-no_warn_duplicate_libraries"
    "LINKER:-force_load,$<TARGET_FILE:gta6rt_shims_core>" "LINKER:-force_load,$<TARGET_FILE:gta6rt_shims_gdk>")

  add_executable(gdk_real_core_test tests/real_core_test.cpp)
  target_include_directories(gdk_real_core_test PRIVATE shared support src "${CMAKE_BINARY_DIR}/portable/generated")
  target_compile_options(gdk_real_core_test PRIVATE -Wno-deprecated-declarations)
  target_link_libraries(gdk_real_core_test PRIVATE gta6rt_shims_core gta6rt_shims_gdk gta6rt_isa gta6rt_os gta6rt_portable
    gta6rt_wincompat gta6rt_dxheaders gta6rt_dxguids)
  target_link_options(gdk_real_core_test PRIVATE "LINKER:-no_warn_duplicate_libraries"
    "LINKER:-force_load,$<TARGET_FILE:gta6rt_shims_core>" "LINKER:-force_load,$<TARGET_FILE:gta6rt_shims_gdk>")
  add_executable(gdk_dstorage_real_core_test tests/dstorage_real_core_test.cpp)
  target_include_directories(gdk_dstorage_real_core_test PRIVATE shared support src)
  target_compile_options(gdk_dstorage_real_core_test PRIVATE -Wno-deprecated-declarations)
  target_link_libraries(gdk_dstorage_real_core_test PRIVATE gta6rt_shims_core gta6rt_shims_gdk gta6rt_isa gta6rt_os gta6rt_portable
    gta6rt_wincompat gta6rt_dxheaders gta6rt_dxguids ZLIB::ZLIB)
  target_link_options(gdk_dstorage_real_core_test PRIVATE "LINKER:-no_warn_duplicate_libraries"
    "LINKER:-force_load,$<TARGET_FILE:gta6rt_shims_core>" "LINKER:-force_load,$<TARGET_FILE:gta6rt_shims_gdk>")
  add_test(NAME gdk_dstorage_real_core COMMAND gdk_dstorage_real_core_test)
  set_tests_properties(gdk_dstorage_real_core PROPERTIES TIMEOUT 120)
  add_test(NAME gdk_real_core COMMAND gdk_real_core_test)
  set_tests_properties(gdk_real_core PROPERTIES TIMEOUT 120)
endif()
