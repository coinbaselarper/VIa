// Registry shim: the console model the title reads from the registry, and a directory-backed store for other keys.
//
// GTA VI does not only call XSystemGetDeviceType. Code linked into the executable (and into libHttpClient.GDK.dll and
// XCurl.dll, which carry the same statically linked GDK helper) opens
//     HKEY_LOCAL_MACHINE\Software\Microsoft\Windows NT\CurrentVersion\OEM
// with RegOpenKeyW / RegOpenKeyExW, reads the DWORD "ConsoleMode" with RegQueryValueExW (0x64 Series X, 0x60 Series
// S; a missing value means a PC, and a PC takes the PC memory and resolution budgets), and calls RegCloseKey. That key
// is answered from LEONIDA_DEVICE_TYPE, the same setting as XSystemGetDeviceType, through a pseudo key.
//
// macOS has no registry, so every other key is stored under a directory (set_store_dir): a key is a folder named by
// its lower-cased path, a value is a file holding a 4-byte type and the data. Keys are created by RegCreateKeyExW and
// RegSetValueExW; opening a missing key is ERROR_FILE_NOT_FOUND.
//
// The ConsoleMode numbers are what VIctory's xsystem.rs maps (GPL-3.0-or-later); 0x68 for the Scarlett devkit is the
// value xbox-emulator's xpal uses.
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <gta6rt/os.h>
#include <gta6rt/shims.h>

#include "console_registry.h"
#include "shims.h"
#include "util.h"

namespace rt::shims::console_registry {
namespace {

constexpr uint32_t kErrorSuccess = 0, kErrorFileNotFound = 2, kErrorAccessDenied = 5, kErrorInvalidHandle = 6,
                   kErrorInvalidParameter = 87, kErrorMoreData = 234;
constexpr uint32_t kRegDword = 4;

constexpr uint64_t kHkcr = 0xFFFFFFFF80000000ull, kHkcu = 0xFFFFFFFF80000001ull, kHklm = 0xFFFFFFFF80000002ull,
                   kHku = 0xFFFFFFFF80000003ull;

const char16_t kOemKey[] = u"Software\\Microsoft\\Windows NT\\CurrentVersion\\OEM";
const char16_t kConsoleMode[] = u"ConsoleMode";

char g_oem_key;
std::mutex g_store_mutex;
std::string g_store_dir;

struct KeyObj {
    std::string hive;
    std::string path;  // lower-case, '/' separated
    bool oem = false;
};

void close_key(void* p) { delete static_cast<KeyObj*>(p); }

uint64_t make_handle(KeyObj* k) {
    uint64_t h = 0;
    if (gta6rt_handle_alloc(GTA6RT_HANDLE_OTHER, k, &close_key, &h) != GTA6RT_OK) {
        delete k;
        return 0;
    }
    return h;
}

KeyObj* key_of(uint64_t h) { return static_cast<KeyObj*>(gta6rt_handle_lookup(h, GTA6RT_HANDLE_OTHER)); }

const char* hive_name(uint64_t key) {
    switch (key) {
    case kHkcr: return "hkcr";
    case kHkcu: return "hkcu";
    case kHklm: return "hklm";
    case kHku: return "hku";
    default: return nullptr;
    }
}

std::string normalise_subkey(const char16_t* sub) {
    std::string s = lower_ascii(narrow(sub));
    for (auto& c : s)
        if (c == '\\') c = '/';
    std::string out;
    bool prev = true;
    for (char c : s) {
        if (c == '/' && prev) continue;
        out.push_back(c);
        prev = c == '/';
    }
    while (!out.empty() && out.back() == '/') out.pop_back();
    if (out.find("..") != std::string::npos) return "\x01";
    return out;
}

std::string encode_name(const std::string& name) {
    if (name.empty()) return "%default";
    std::string out;
    for (unsigned char c : name) {
        if (std::isalnum(c) || c == '_' || c == '-' || c == ' ') out.push_back(static_cast<char>(c));
        else {
            char b[4];
            std::snprintf(b, sizeof b, "%%%02X", c);
            out += b;
        }
    }
    return out;
}

std::string key_dir(const KeyObj& k) {
    std::lock_guard lock(g_store_mutex);
    if (g_store_dir.empty()) return {};
    return g_store_dir + "/" + k.hive + (k.path.empty() ? "" : "/" + k.path);
}

bool ours(uint64_t key, const char16_t* sub) {
    uint32_t mode;
    return key == kHklm && sub && u16ieq(sub, kOemKey) && console_mode(mode);
}

uint32_t open_key(uint64_t key, const char16_t* sub, uint64_t* result, bool create) {
    if (!result) return kErrorInvalidParameter;
    if (const KeyObj* parent = key_of(key)) {
        if (parent->oem) {
            if (sub && *sub) return kErrorFileNotFound;
            auto* k = new KeyObj(*parent);
            *result = make_handle(k);
            return *result ? kErrorSuccess : 8;
        }
    }
    if (ours(key, sub)) {
        auto* k = new KeyObj;
        k->hive = "hklm";
        k->oem = true;
        *result = make_handle(k);
        return *result ? kErrorSuccess : 8;
    }
    auto* k = new KeyObj;
    if (const char* h = hive_name(key)) {
        k->hive = h;
    } else if (const KeyObj* parent = key_of(key)) {
        k->hive = parent->hive;
        k->path = parent->path;
    } else {
        delete k;
        return kErrorInvalidHandle;
    }
    const std::string rel = sub ? normalise_subkey(sub) : std::string();
    if (rel == "\x01") {
        delete k;
        return kErrorInvalidParameter;
    }
    if (!rel.empty()) k->path += (k->path.empty() ? "" : "/") + rel;
    const std::string dir = key_dir(*k);
    if (dir.empty()) {
        if (!create && !rel.empty()) {
            delete k;
            return kErrorFileNotFound;
        }
    } else {
        struct stat st;
        if (::stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            if (!create) {
                delete k;
                return kErrorFileNotFound;
            }
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if (ec) {
                delete k;
                return kErrorAccessDenied;
            }
        }
    }
    *result = make_handle(k);
    return *result ? kErrorSuccess : 8;
}

uint32_t WINAPI reg_open_key_w(uint64_t key, const char16_t* sub, uint64_t* result) { return open_key(key, sub, result, false); }

uint32_t WINAPI reg_open_key_ex_w(uint64_t key, const char16_t* sub, uint32_t options, uint32_t desired, uint64_t* result) {
    (void)options; (void)desired;
    return open_key(key, sub, result, false);
}

uint32_t WINAPI reg_create_key_ex_w(uint64_t key, const char16_t* sub, uint32_t reserved, char16_t* cls, uint32_t options,
                                    uint32_t desired, void* security, uint64_t* result, uint32_t* disposition) {
    (void)reserved; (void)cls; (void)options; (void)desired; (void)security;
    uint64_t probe = 0;
    const bool existed = open_key(key, sub, &probe, false) == kErrorSuccess;
    if (existed) gta6rt_handle_close(probe);
    const uint32_t rc = open_key(key, sub, result, true);
    if (rc == kErrorSuccess && disposition) *disposition = existed ? 2 : 1;
    return rc;
}

bool read_value(const std::string& path, uint32_t& type, std::vector<uint8_t>& data) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.read(reinterpret_cast<char*>(&type), 4);
    if (in.gcount() != 4) return false;
    data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

uint32_t WINAPI reg_query_value_ex_w(uint64_t key, const char16_t* name, uint32_t* reserved, uint32_t* type, uint8_t* data,
                                     uint32_t* size) {
    (void)reserved;
    KeyObj* k = key_of(key);
    if (!k) return kErrorInvalidHandle;
    uint32_t vtype = 0;
    std::vector<uint8_t> bytes;
    if (k->oem) {
        uint32_t mode;
        if (!name || !u16ieq(name, kConsoleMode) || !console_mode(mode)) return kErrorFileNotFound;
        vtype = kRegDword;
        bytes.resize(4);
        std::memcpy(bytes.data(), &mode, 4);
        static std::atomic<uint32_t> logged{0};
        if (logged.exchange(mode) != mode)
            RTLOG(INFO, "registry: ConsoleMode answered 0x%x (LEONIDA_DEVICE_TYPE=%u)", mode, configured_device_type());
    } else {
        const std::string dir = key_dir(*k);
        if (dir.empty() || !read_value(dir + "/" + encode_name(name ? narrow(name) : std::string()), vtype, bytes))
            return kErrorFileNotFound;
    }
    if (data && !size) return kErrorInvalidParameter;
    if (type) *type = vtype;
    if (!size) return kErrorSuccess;
    const uint32_t room = *size;
    *size = static_cast<uint32_t>(bytes.size());
    if (!data) return kErrorSuccess;
    if (room < bytes.size()) return kErrorMoreData;
    std::memcpy(data, bytes.data(), bytes.size());
    return kErrorSuccess;
}

uint32_t WINAPI reg_set_value_ex_w(uint64_t key, const char16_t* name, uint32_t reserved, uint32_t type, const uint8_t* data,
                                   uint32_t size) {
    (void)reserved;
    KeyObj* k = key_of(key);
    if (!k) return kErrorInvalidHandle;
    if (k->oem) return kErrorAccessDenied;
    if (!data && size) return kErrorInvalidParameter;
    const std::string dir = key_dir(*k);
    if (dir.empty()) return kErrorAccessDenied;
    std::ofstream out(dir + "/" + encode_name(name ? narrow(name) : std::string()), std::ios::binary | std::ios::trunc);
    if (!out) return kErrorAccessDenied;
    out.write(reinterpret_cast<const char*>(&type), 4);
    if (size) out.write(reinterpret_cast<const char*>(data), size);
    return out ? kErrorSuccess : kErrorAccessDenied;
}

uint32_t WINAPI reg_delete_value_w(uint64_t key, const char16_t* name) {
    KeyObj* k = key_of(key);
    if (!k) return kErrorInvalidHandle;
    if (k->oem) return kErrorAccessDenied;
    const std::string dir = key_dir(*k);
    if (dir.empty() || ::unlink((dir + "/" + encode_name(name ? narrow(name) : std::string())).c_str()) != 0)
        return kErrorFileNotFound;
    return kErrorSuccess;
}

uint32_t WINAPI reg_close_key(uint64_t key) {
    if (!key_of(key)) return kErrorInvalidHandle;
    gta6rt_handle_close(key);
    return kErrorSuccess;
}

}  // namespace

uint64_t pseudo_key() { return reinterpret_cast<uintptr_t>(&g_oem_key); }

uint32_t configured_device_type() {
    const char* s = gta6rt_env_get("LEONIDA_DEVICE_TYPE");
    if (s && *s) {
        char* end = nullptr;
        const long v = std::strtol(s, &end, 10);
        if (end != s && v >= 0 && v <= 8) return static_cast<uint32_t>(v);
        if (end == s) return 0;
    }
    return 7;
}

bool console_mode(uint32_t& mode) {
    switch (configured_device_type()) {
    case 6: mode = 0x60; return true;
    case 7: mode = 0x64; return true;
    case 8: mode = 0x68; return true;
    default: return false;
    }
}

void set_store_dir(const std::string& dir) {
    std::lock_guard lock(g_store_mutex);
    g_store_dir = dir;
    if (!dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    }
}

}  // namespace rt::shims::console_registry

namespace {
using namespace rt::shims::console_registry;
#define REG_ENTRIES(dll)                                                                                    \
    SHIM(dll, "RegOpenKeyW", reg_open_key_w), SHIM(dll, "RegOpenKeyExW", reg_open_key_ex_w),               \
    SHIM(dll, "RegCreateKeyExW", reg_create_key_ex_w), SHIM(dll, "RegQueryValueExW", reg_query_value_ex_w), \
    SHIM(dll, "RegSetValueExW", reg_set_value_ex_w), SHIM(dll, "RegDeleteValueW", reg_delete_value_w),     \
    SHIM(dll, "RegCloseKey", reg_close_key)
}  // namespace

SHIM_TABLE(console_registry,
    REG_ENTRIES("advapi32.dll"),
    REG_ENTRIES("api-ms-win-core-registry-l1-1-0.dll"),
);

namespace rt::shims {
namespace {

constexpr ConsoleProfile kSeriesS{"series-s", 6, 7776, 8};
constexpr ConsoleProfile kSeriesX{"series-x", 7, 13824, 14};
std::atomic<int> g_profile{-1};
std::string g_profile_reason;

uint64_t host_memory() {
    uint64_t total = 0;
    size_t n = sizeof total;
    if (sysctlbyname("hw.memsize", &total, &n, nullptr, 0) != 0) total = 0;
    return total;
}

int parse_profile(const char* v) {
    if (!v || !*v) return -1;
    std::string s;
    for (const char* p = v; *p; ++p) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
    if (s == "series-s" || s == "s" || s == "lockhart") return 0;
    if (s == "series-x" || s == "x" || s == "anaconda") return 1;
    if (s == "auto") return 2;
    return -2;
}

int resolve(int choice, std::string& why) {
    if (choice == 0 || choice == 1) {
        why = "selected";
        return choice;
    }
    const uint64_t mem = host_memory();
    why = "auto: " + std::to_string(mem >> 30) + " GiB of physical memory";
    return mem && mem <= (uint64_t{32} << 30) ? 0 : 1;
}

}  // namespace

bool select_console_profile(const char* name) {
    const int c = parse_profile(name);
    if (c < 0) return false;
    std::string why;
    const int r = resolve(c, why);
    g_profile_reason = why;
    g_profile.store(r);
    return true;
}

const ConsoleProfile& console_profile() {
    int p = g_profile.load();
    if (p < 0) {
        const char* env = gta6rt_env_get("LEONIDA_CONSOLE_PROFILE");
        int c = parse_profile(env);
        if (c == -2) {
            RTLOG(WARN, "LEONIDA_CONSOLE_PROFILE=%s: expected series-s, series-x or auto; using auto", env);
            c = 2;
        }
        std::string why;
        const int r = resolve(c < 0 ? 2 : c, why);
        int expected = -1;
        if (g_profile.compare_exchange_strong(expected, r)) g_profile_reason = env && c != 2 ? "LEONIDA_CONSOLE_PROFILE" : why;
        p = g_profile.load();
    }
    return p == 0 ? kSeriesS : kSeriesX;
}

void apply_console_profile() {
    const ConsoleProfile& p = console_profile();
    const bool dt_set = gta6rt_env_get("LEONIDA_DEVICE_TYPE") != nullptr;
    const bool mem_set = gta6rt_env_get("LEONIDA_TITLE_MEMORY_MB") != nullptr;
    if (!dt_set) gta6rt_env_set("LEONIDA_DEVICE_TYPE", std::to_string(p.device_type).c_str());
    if (!mem_set) gta6rt_env_set("LEONIDA_TITLE_MEMORY_MB", std::to_string(p.title_memory_mib).c_str());
    RTLOG(INFO, "console profile %s (%s): device type %s, title memory %s MiB, xmem pool %u GiB%s", p.name,
          g_profile_reason.c_str(), gta6rt_env_get("LEONIDA_DEVICE_TYPE"), gta6rt_env_get("LEONIDA_TITLE_MEMORY_MB"),
          p.pool_gib, dt_set || mem_set ? " (LEONIDA_DEVICE_TYPE / LEONIDA_TITLE_MEMORY_MB from the environment win)" : "");
}

}  // namespace rt::shims

extern "C" gta6rt_status gta6rt_shims_set_console_profile(const char* name) {
    return rt::shims::select_console_profile(name) ? GTA6RT_OK : GTA6RT_E_INVALID_ARG;
}
