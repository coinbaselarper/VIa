#pragma once

/*
 * gta6rt_loader: PE32+ mapping for the title exe and packaged DLLs (S1.4). Implemented by loader/.
 * Consumed by host/ (boot sequence) and shims_core/ (LoadLibrary of package DLLs).
 * Measured in S0.3: MAP_FIXED at 0x140000000 works under Rosetta; DIR64 and ABSOLUTE are the only
 * relocation types; no delay imports in the exe; static TLS uses _tls_index 0; 587 imports / 68 DLLs,
 * three of them MSVCP140 data imports that must bind to storage, not code.
 */

#include <gta6rt/abi.h>
#include <gta6rt/isa.h>

GTA6RT_API_BEGIN

#define GTA6RT_LOADER_API_VERSION 2

typedef struct gta6rt_image gta6rt_image;

#define GTA6RT_MAP_ALLOW_RELOCATE 1u /* map elsewhere if the preferred base is taken */
#define GTA6RT_MAP_FORCE_RELOCATE 2u /* test path: never use the preferred base (S03_FORCE_RELOC) */

typedef struct gta6rt_map_options {
    uint32_t flags;
    gta6rt_gva base_override; /* 0 = preferred ImageBase */
} gta6rt_map_options;

typedef struct gta6rt_image_info {
    gta6rt_gva base;
    gta6rt_gva preferred_base;
    uint64_t size;
    gta6rt_gva entry; /* 0 if none */
    uint64_t stack_reserve;
    uint64_t stack_commit;
    uint16_t machine;
    uint16_t subsystem;
    uint16_t characteristics;
    uint16_t dll_characteristics;
    uint32_t section_count;
    uint32_t timestamp;
    const char* path_utf8;
} gta6rt_image_info;

/* Step 1: map headers and sections at the chosen base, with writable protections; no fixups yet. */
gta6rt_status gta6rt_loader_map_file(const char* path_utf8, const gta6rt_map_options* opt, gta6rt_image** out);
gta6rt_status gta6rt_loader_map_memory(const void* file, size_t size, const char* name, const gta6rt_map_options* opt,
                                       gta6rt_image** out);
gta6rt_status gta6rt_loader_info(const gta6rt_image* img, gta6rt_image_info* out);

/* Step 2: apply base relocations if base != preferred_base (no-op otherwise). */
gta6rt_status gta6rt_loader_relocate(gta6rt_image* img);

/* Step 3: bind imports. The resolver is called once per imported symbol (regular and delay imports). */
typedef enum gta6rt_import_kind { GTA6RT_IMPORT_CODE = 1, GTA6RT_IMPORT_DATA = 2 } gta6rt_import_kind;

typedef struct gta6rt_import_ref {
    const char* dll;  /* as written in the image, e.g. "KERNEL32.dll" or "api-ms-win-core-synch-l1-2-0.dll" */
    const char* name; /* NULL when by ordinal */
    uint16_t ordinal;
    uint16_t hint;
    uint32_t delay;   /* from the delay-import directory */
    gta6rt_import_kind kind_guess; /* DATA for MSVC data symbols (mangled storage-class digit @@[0-3]) */
} gta6rt_import_ref;

typedef struct gta6rt_import_binding {
    gta6rt_gva address; /* guest-callable thunk (code) or storage address (data) */
} gta6rt_import_binding;

/* Return GTA6RT_OK with *out filled, or GTA6RT_E_NOT_FOUND to let the loader install a logging trap thunk
 * (gta6rt_isa_make_thunk with GTA6RT_THUNK_TRAP). Default stubs that return 0 are not allowed (S0.3). */
typedef gta6rt_status (*gta6rt_import_resolver)(void* user, const gta6rt_import_ref* ref, gta6rt_import_binding* out);

typedef struct gta6rt_bind_stats {
    uint32_t total;
    uint32_t bound;
    uint32_t trapped;
    uint32_t data;
    uint32_t data_zero_storage; /* v2: data imports the resolver did not serve; bound to zeroed host storage */
} gta6rt_bind_stats;

gta6rt_status gta6rt_loader_bind_imports(gta6rt_image* img, gta6rt_import_resolver resolver, void* user,
                                         gta6rt_bind_stats* out_stats);

/* Step 4: patches (e.g. __chkstk -> ret at file offset 0x5646540, S0.6), then final section protections. */
gta6rt_status gta6rt_loader_patch_rva(gta6rt_image* img, uint32_t rva, const void* bytes, size_t size);
gta6rt_status gta6rt_loader_file_offset_to_rva(const gta6rt_image* img, uint64_t file_offset, uint32_t* out_rva);
gta6rt_status gta6rt_loader_finalize_protections(gta6rt_image* img);

/* Exception directory, for gta6rt_seh_add_function_table. */
gta6rt_status gta6rt_loader_exception_table(const gta6rt_image* img, const void** out_table, uint32_t* out_count);

/* Static TLS. index_address receives the module's TLS index (_tls_index). */
typedef struct gta6rt_tls_info {
    gta6rt_gva template_start;
    uint64_t template_size;
    uint64_t zero_fill;
    uint32_t alignment;
    gta6rt_gva index_address;
    const gta6rt_gva* callbacks; /* null-terminated, may be NULL */
} gta6rt_tls_info;

gta6rt_status gta6rt_loader_tls_info(const gta6rt_image* img, gta6rt_tls_info* out); /* GTA6RT_E_NOT_FOUND if no TLS */
/* Process-wide TLS slot assignment across images (exe gets index 0). */
gta6rt_status gta6rt_loader_tls_assign_index(gta6rt_image* img, uint32_t* out_index);
/* The loader does not own any per-thread ThreadLocalStoragePointer array. tls_assign_index registers the image's
 * template with gta6rt_isa_tls_register_image (ctor/dtor allocate and free the per-thread block), and unmap
 * unregisters it. isa builds and grows the array of every attached thread, including threads it attaches itself,
 * so threads must be attached with gta6rt_teb_desc.tls_pointer == NULL. */
/* Run TLS callbacks for reason (DLL_PROCESS_ATTACH = 1, ...) through gta6rt_isa_call_guest. */
gta6rt_status gta6rt_loader_run_tls_callbacks(gta6rt_image* img, uint32_t reason);

/* Exports (package DLLs), by name or ordinal. 0 if absent; forwarders are resolved via the resolver. */
gta6rt_gva gta6rt_loader_get_export(const gta6rt_image* img, const char* name, uint32_t ordinal);

/* Entry address (exe entry or DllMain); calling it is host/'s job (gta6rt_isa_run_entry). */
gta6rt_gva gta6rt_loader_entry(const gta6rt_image* img);

void gta6rt_loader_unmap(gta6rt_image* img);

/* ======================================================================================================
 * v2 additions (S1.4)
 * ==================================================================================================== */

/* Host services the loader uses. NULL members use the gta6rt_os / gta6rt_isa implementation. Replacing them is
 * for tests that run before those modules exist (no guest code runs) and for the host's own diagnostics. */
typedef struct gta6rt_loader_host_ops {
    uint32_t size; /* sizeof(gta6rt_loader_host_ops) */
    gta6rt_status (*vm_reserve)(void* addr, size_t size, uint32_t flags, void** out_base);
    gta6rt_status (*vm_commit)(void* addr, size_t size, uint32_t prot);
    gta6rt_status (*vm_protect)(void* addr, size_t size, uint32_t prot, uint32_t* out_old_prot);
    gta6rt_status (*vm_release)(void* allocation_base);
    size_t (*vm_page_size)(void);
    gta6rt_status (*file_map_readonly)(const char* path_utf8, const void** out_data, size_t* out_size);
    void (*file_unmap)(const void* data, size_t size);
    gta6rt_status (*make_thunk)(const gta6rt_thunk_desc* desc, gta6rt_gva* out);
    gta6rt_status (*patch_code)(gta6rt_gva addr, const void* bytes, size_t size);
    void (*flush_icache)(gta6rt_gva addr, size_t size);
    gta6rt_status (*call_guest)(gta6rt_gva target, const gta6rt_guest_args* args, gta6rt_guest_ret* out);
    gta6rt_status (*tls_register_image)(uint32_t tls_index, gta6rt_tls_block_ctor ctor, gta6rt_tls_block_dtor dtor,
                                        void* user);
    gta6rt_status (*tls_unregister_image)(uint32_t tls_index);
} gta6rt_loader_host_ops;

/* NULL restores the defaults. Call before the first map. */
void gta6rt_loader_set_host_ops(const gta6rt_loader_host_ops* ops);

typedef struct gta6rt_section_info {
    char name[9];
    uint32_t rva;
    uint32_t virtual_size;
    uint32_t raw_size;
    uint32_t raw_offset;
    uint32_t characteristics; /* IMAGE_SCN_* */
} gta6rt_section_info;

gta6rt_status gta6rt_loader_section(const gta6rt_image* img, uint32_t index, gta6rt_section_info* out);

typedef struct gta6rt_image_stats {
    uint32_t section_count;
    uint32_t relocated;      /* mapped away from the preferred base */
    uint64_t reloc_entries;  /* table entries excluding ABSOLUTE padding */
    uint64_t reloc_dir64;
    uint64_t reloc_highlow;
    uint64_t reloc_padding;  /* ABSOLUTE entries */
    uint64_t reloc_applied;  /* entries patched by gta6rt_loader_relocate (0 at the preferred base) */
    uint32_t import_dlls;
    uint32_t import_total;
    uint32_t import_by_ordinal;
    uint32_t import_delay;
    uint32_t export_total;
    uint32_t export_forwarders;
    uint32_t exception_entries;
    uint32_t tls_callbacks;
    uint32_t tls_present;
    uint32_t tls_index;      /* 0xFFFFFFFF until assigned */
} gta6rt_image_stats;

gta6rt_status gta6rt_loader_stats(const gta6rt_image* img, gta6rt_image_stats* out);

/* Walk regular and delay imports in directory order without binding. Return nonzero from the visitor to stop. */
typedef int (*gta6rt_import_visitor)(void* user, const gta6rt_import_ref* ref, uint64_t iat_rva);
gta6rt_status gta6rt_loader_enumerate_imports(const gta6rt_image* img, gta6rt_import_visitor fn, void* user);

/* Walk exports (by name and ordinal-only). forwarder is NULL for ordinary exports. */
typedef int (*gta6rt_export_visitor)(void* user, const char* name, uint32_t ordinal, uint32_t rva,
                                     const char* forwarder);
gta6rt_status gta6rt_loader_enumerate_exports(const gta6rt_image* img, gta6rt_export_visitor fn, void* user);

/* The MSVC data-symbol heuristic bind_imports applies to names ("?x@@3HA" is data, "?f@@YAXXZ" is code). */
gta6rt_import_kind gta6rt_loader_guess_import_kind(const char* name);

/* Section flags of the section containing rva (IMAGE_SCN_*), 0 if none. */
uint32_t gta6rt_loader_rva_section_flags(const gta6rt_image* img, uint32_t rva);

/* Static TLS slot assigned by gta6rt_loader_tls_assign_index; 0xFFFFFFFF if none. */
uint32_t gta6rt_loader_tls_index(const gta6rt_image* img);

/* Run on every thread that runs guest code, after gta6rt_isa_thread_attach / before gta6rt_isa_thread_detach:
 * delivers DLL_THREAD_ATTACH / DLL_THREAD_DETACH to the TLS callbacks and DllMain of every initialised image. */
gta6rt_status gta6rt_loader_notify_thread(uint32_t reason);

/* Image containing a guest address, NULL if none. */
const gta6rt_image* gta6rt_loader_find_image(gta6rt_gva addr);
/* "game_scarlett_master.exe+0x5a91bd0" or "0x1234" into buf; always NUL-terminated. */
void gta6rt_loader_describe_address(gta6rt_gva addr, char* buf, size_t buf_size);

/* ---- package directory: bundled PE DLLs ------------------------------------------------------------ */

/* flags = 0 registers each loaded DLL with gta6rt_module_register and its exception table with
 * gta6rt_seh_add_function_table. The NO_ bits turn that off (tests that run before os/seh exist). */
#define GTA6RT_PKG_NO_MODULE_REGISTRATION 1u
#define GTA6RT_PKG_NO_SEH_REGISTRATION 2u

typedef struct gta6rt_package_desc {
    uint32_t size; /* sizeof(gta6rt_package_desc) */
    const char* dir_utf8;
    /* Tried first for every import of every DLL not in the exclusive list (the shim table). NULL = none. */
    gta6rt_import_resolver resolver;
    void* resolver_user;
    /* DLLs whose every export comes from the package copy, never from the resolver (shared mutable state or
     * exception handlers must not be split between a shim and the real DLL). NULL = default list: vcruntime140*,
     * msvcp140*, concrt140, vccorlib140, vcomp140, Bink2Scarlett, libHttpClient.GDK, XCurl, amd_fidelityfx_*.
     * XboxBvhBuilder is not exclusive: a shim may serve it, the package copy is the fallback. */
    const char* const* exclusive_dlls;
    uint32_t exclusive_count; /* used only when exclusive_dlls != NULL */
    uint32_t flags;           /* GTA6RT_PKG_NO_* */
} gta6rt_package_desc;

gta6rt_status gta6rt_loader_package_init(const gta6rt_package_desc* desc);
void gta6rt_loader_package_shutdown(void);

#define GTA6RT_PKG_LOAD_DYNAMIC 1u /* LoadLibrary at run time: DllMain gets lpvReserved NULL */

/* Map, relocate, bind (loading dependencies first) and protect a package DLL by name ("vcruntime140.dll" or
 * "VCRUNTIME140"). Idempotent and case-insensitive. Does not run DllMain: call gta6rt_loader_package_initialize. */
gta6rt_status gta6rt_loader_package_load(const char* dll_name, uint32_t flags, gta6rt_image** out);
/* Already loaded image, or NULL. */
gta6rt_image* gta6rt_loader_package_find(const char* dll_name);
/* 1 if the package directory holds that DLL (loaded or not). */
int gta6rt_loader_package_exists(const char* dll_name);
/* Run TLS callbacks and DllMain(DLL_PROCESS_ATTACH) of loaded, not yet initialised DLLs, dependencies first. */
gta6rt_status gta6rt_loader_package_initialize(void);
/* Process-detach pass (TLS callbacks and DllMain(DLL_PROCESS_DETACH), reverse order). The host decides whether to run
 * guest code at exit; gta6rt_loader_package_shutdown unmaps without calling the guest. */
gta6rt_status gta6rt_loader_package_detach(void);
/* DisableThreadLibraryCalls: the image at base gets no DllMain thread notifications. */
gta6rt_status gta6rt_loader_disable_thread_notifications(gta6rt_gva base);
/* Images in dependency order (dependencies first). Returns the count; fills up to max entries. */
uint32_t gta6rt_loader_package_images(gta6rt_image** out, uint32_t max);
/* The import resolver to give gta6rt_loader_bind_imports for the exe: the package resolver (shims first unless the
 * DLL is exclusive), then package exports, loading DLLs on demand. user is ignored. */
gta6rt_status gta6rt_loader_package_import_resolver(void* user, const gta6rt_import_ref* ref,
                                                    gta6rt_import_binding* out);

/* ---- diagnostics (port of thread_stacks.cpp) -------------------------------------------------------- */

/* One line per frame ("#0 game_scarlett_master.exe+0x1234") from gta6rt_seh_backtrace over ctx. */
size_t gta6rt_loader_format_backtrace(const gta6rt_x64_context* ctx, char* buf, size_t buf_size, uint32_t max_frames);

typedef struct gta6rt_thread_stack_sample {
    uint32_t thread_id;
    char label[64];
    gta6rt_x64_context ctx;
} gta6rt_thread_stack_sample;
/* Captures the guest context of every other guest thread (suspend, copy, resume). There is no such primitive in
 * os.h / isa.h yet; until a provider is set the sampler reports that it cannot sample. */
typedef uint32_t (*gta6rt_stack_provider)(void* user, gta6rt_thread_stack_sample* out, uint32_t max);
void gta6rt_loader_set_stack_provider(gta6rt_stack_provider fn, void* user);
/* "5,10,30": whole seconds since start, ascending, deduplicated. 0 on success. */
int gta6rt_loader_parse_stack_times(const char* text, uint32_t* out, uint32_t max, uint32_t* out_count);
/* Reads GTA6RT_STACKS (leonida: LEONIDA_STACKS) and starts the sampler thread; no-op if unset. */
gta6rt_status gta6rt_loader_start_stack_sampler(void);
void gta6rt_loader_stop_stack_sampler(void);

GTA6RT_API_END
