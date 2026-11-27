#pragma once

/*
 * gta6rt_shims: the host-served DLL export table (S1.5a/b, S1.6). Implemented by shims_core/; shims_gdk/
 * and d3d12_vulkan/ add their DLLs through gta6rt_shims_register_dll. host/ passes
 * gta6rt_shims_import_resolver to gta6rt_loader_bind_imports.
 */

#include <gta6rt/abi.h>
#include <gta6rt/loader.h>

GTA6RT_API_BEGIN

#define GTA6RT_SHIMS_API_VERSION 4 /* 3: console profile; 4: gta6rt_d3d12_vulkan_at_exit */

typedef enum gta6rt_shim_export_kind {
    GTA6RT_SHIM_CODE_NATIVE = 1, /* GTA6RT_GUEST_ABI function pointer (x86_64 fast path) */
    GTA6RT_SHIM_CODE_FRAME = 2,  /* gta6rt_frame_handler (both arches) */
    GTA6RT_SHIM_DATA = 3         /* address of host storage the guest reads/writes directly */
} gta6rt_shim_export_kind;

typedef struct gta6rt_shim_export {
    const char* name; /* NULL terminates the table */
    uint16_t ordinal; /* 0 = by name only */
    gta6rt_shim_export_kind kind;
    const void* target;
} gta6rt_shim_export;

/* exports must stay valid for the process lifetime. Aliases (api-ms-win-*) are separate registrations. */
gta6rt_status gta6rt_shims_register_dll(const char* dll_name, const gta6rt_shim_export* exports);
gta6rt_status gta6rt_shims_init(void);
/* Matches gta6rt_import_resolver. user is ignored. */
gta6rt_status gta6rt_shims_import_resolver(void* user, const gta6rt_import_ref* ref, gta6rt_import_binding* out);

/* ---- S1.5a host controls (shims_core/, API version 2) ------------------------------------------------------ */

/* Roots of the console file system the title sees: "GAME:" and "G:\\" map under game_dir; every write, delete and
 * "T:\\" go to overlay_dir (created on first write). Also places the registry store in <overlay>/.gta6rt-registry. */
gta6rt_status gta6rt_shims_configure_paths(const char* game_dir_utf8, const char* overlay_dir_utf8);
/* GetCommandLine and the CRT's argv: the program name (as the title sees it) and its arguments. */
gta6rt_status gta6rt_shims_set_command_line(const char* program_utf8, const char* const* args_utf8, size_t count);
/* Runs the mapped exe's entry point on a new guest thread and returns the title's exit code (the entry's return value,
 * or what it passed to exit/ExitProcess; 3 for abort/terminate). */
int gta6rt_shims_run_entry(gta6rt_gva entry, size_t stack_bytes);
/* The title's window was closed: runs its suspend notifications (about 1 s), then ends the run. */
void gta6rt_shims_request_suspend(void);
/* A guest thread is ending: runs the exe's registered thread-local atexit callback. Call from the on_exit hook. */
void gta6rt_shims_thread_detach(void);
/* Prints every import that was called but not served (named stubs), and returns how many calls hit one. */
size_t gta6rt_shims_report_unimplemented(void);
/* API 3. The console the title is told it runs on: "series-s" (device type 6, 7,776 MiB title memory), "series-x"
 * (device type 7, 13,824 MiB) or "auto" (Series S when the host has 32 GiB of physical memory or less). Call before
 * gta6rt_shims_init; without a call LEONIDA_CONSOLE_PROFILE is read, then auto. The profile fills
 * LEONIDA_DEVICE_TYPE and LEONIDA_TITLE_MEMORY_MB when they are unset and sizes the xmem pool. GTA6RT_E_INVALID_ARG
 * for another name. */
gta6rt_status gta6rt_shims_set_console_profile(const char* name);

/* Implemented by shims_gdk/ and d3d12_vulkan/: register their DLLs with gta6rt_shims_register_dll. */
gta6rt_status gta6rt_shims_gdk_register(void);
gta6rt_status gta6rt_d3d12_vulkan_register(void);
/* API 4. d3d12_vulkan/: before a process ends with its D3D12 device alive (the host's watchdog, the title's exit):
 * stops the pipeline compiler threads (waiting up to wait_ms for a running build) and writes the session pipeline cache
 * (LEONIDA_PIPELINE_CACHE_DIR/pipeline_cache.session.bin). GTA6RT_E_NOT_FOUND when nothing was written. */
gta6rt_status gta6rt_d3d12_vulkan_at_exit(uint32_t wait_ms);

GTA6RT_API_END
