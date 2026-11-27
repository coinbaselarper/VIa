#pragma once

#include <gta6rt/loader.h>
#include <gta6rt/os.h>
#include <gta6rt/seh.h>
#include <gta6rt/winnt_image.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ldr {

struct Section {
    char name[9];
    uint32_t rva, vsize, raw_size, raw_offset, characteristics;
};

struct Export {
    uint32_t rva = 0;
    uint32_t ordinal = 0;
    bool named = false;
    std::string forwarder;
};

struct TlsSlotInfo;

}  // namespace ldr

struct gta6rt_image {
    std::string name;
    std::string path;
    uint8_t* base = nullptr;
    uint64_t preferred_base = 0;
    uint32_t size = 0;
    uint32_t headers_size = 0;
    uint32_t entry_rva = 0;
    uint16_t machine = 0, subsystem = 0, characteristics = 0, dll_characteristics = 0;
    uint32_t timestamp = 0;
    uint64_t stack_reserve = 0, stack_commit = 0;
    uint64_t file_size = 0;
    uint32_t data_dir_count = 0;
    IMAGE_DATA_DIRECTORY dirs[IMAGE_NUMBEROF_DIRECTORY_ENTRIES] = {};
    std::vector<ldr::Section> sections;

    bool relocated = false;
    bool imports_bound = false;
    bool finalized = false;
    uint64_t reloc_applied = 0;

    bool exports_parsed = false;
    std::unordered_map<std::string, ldr::Export> exports_by_name;
    std::unordered_map<uint32_t, ldr::Export> exports_by_ordinal;
    uint32_t export_forwarders = 0;

    uint32_t tls_index = 0xFFFFFFFFu;

    bool is_dll() const { return (characteristics & IMAGE_FILE_DLL) != 0; }

    bool in_package = false;
    bool dynamic_load = false;
    bool initialised = false;
    bool init_started = false;
    bool dllmain_ran = false;
    bool thread_notify = false;
    bool thread_calls_disabled = false;
    gta6rt_module* module = nullptr;
    bool seh_registered = false;
    const void* seh_table = nullptr;
};

namespace ldr {

using Image = gta6rt_image;

const gta6rt_loader_host_ops& ops();

void log_error(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log_warn(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log_info(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log_debug(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

std::string lower(std::string s);
bool range_ok(const Image* img, uint64_t rva, uint64_t len);
const IMAGE_NT_HEADERS64* nt_headers(const Image* img);

gta6rt_status parse_exports(Image* img);
gta6rt_status exception_table(Image* img, const gta6rt_runtime_function** table, uint32_t* count);

const Export* find_export(Image* img, const char* name, uint32_t ordinal);

void registry_add(Image* img);
void registry_remove(Image* img);
std::vector<Image*> registry_snapshot();

gta6rt_gva resolve_forwarder(Image* img, const std::string& spec);

gta6rt_status tls_assign(Image* img, uint32_t* out_index);
void tls_release(Image* img);

gta6rt_gva trap_thunk(const std::string& dll, const std::string& sym);
void* zero_storage(size_t size);

gta6rt_status enumerate_imports(const Image* img, gta6rt_import_visitor fn, void* user);

void package_unmap_hook(Image* img);

}  // namespace ldr
