// The registry answer for the console model plus a directory-backed store for every other key.
//
// HKLM\Software\Microsoft\Windows NT\CurrentVersion\OEM, value ConsoleMode (a DWORD) is how GTA VI tells a console
// from a PC; it is answered from LEONIDA_DEVICE_TYPE through a pseudo key (0x60 Series S, 0x64 Series X, 0x68 devkit;
// no value for a PC or an Xbox One). Everything else lives in <overlay>/.gta6rt-registry/<hive>/<key>/, one file per
// value (4-byte type, then the data), so a title that writes a key reads it back across runs.
#pragma once

#include <cstdint>
#include <string>

namespace rt::shims::console_registry {

// The pseudo key handed out for the OEM key. Never a store handle.
uint64_t pseudo_key();

// ConsoleMode for LEONIDA_DEVICE_TYPE (default Series X), or false when the type has none.
bool console_mode(uint32_t& mode);
uint32_t configured_device_type();

// Where the store keeps its files; empty disables persistence (every non-OEM key then reads as missing).
void set_store_dir(const std::string& dir_utf8);

}  // namespace rt::shims::console_registry

namespace rt::shims {

// The console profile (shims.h gta6rt_shims_set_console_profile): Series S on hosts with 32 GiB or less, as the
// cool_fox assessment measured the forks needing 34-46 GB with Series X.
struct ConsoleProfile {
    const char* name;
    uint32_t device_type;
    uint32_t title_memory_mib;
    uint32_t pool_gib;
};
const ConsoleProfile& console_profile();
bool select_console_profile(const char* name);
// Logs the profile and publishes LEONIDA_DEVICE_TYPE / LEONIDA_TITLE_MEMORY_MB where unset (gta6rt_shims_init).
void apply_console_profile();

}  // namespace rt::shims
