#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <gta6rt/shims.h>

#include "shim_call.h"
#include "test.h"

using tcall::call;

namespace {
uint64_t g_thread_exit_args[3];
uint64_t WINAPI thread_exit_cb(uint64_t module, uint64_t reason, uint64_t reserved) {
    g_thread_exit_args[0] = module;
    g_thread_exit_args[1] = reason;
    g_thread_exit_args[2] = reserved;
    return 0;
}
}  // namespace

TEST(api_configure_paths_command_line_and_thread_exit) {
    namespace fsys = std::filesystem;
    const fsys::path root = fsys::temp_directory_path() / "gta6rt-api-test";
    fsys::remove_all(root);
    fsys::create_directories(root / "game");
    std::ofstream(root / "game" / "f.txt") << "x";
    CHECK_EQ(gta6rt_shims_configure_paths((root / "game").c_str(), (root / "overlay").c_str()), GTA6RT_OK);
    CHECK(call<uint32_t>("kernel32.dll", "GetFileAttributesW", u"GAME:\\f.txt") != 0xFFFFFFFFu);
    uint64_t key = 0;
    const uint32_t v = 9;
    CHECK_EQ(call<uint32_t>("advapi32.dll", "RegCreateKeyExW", uint64_t{0xFFFFFFFF80000001ull}, u"Software\\X", 0u, static_cast<char16_t*>(nullptr), 0u, 0u, static_cast<void*>(nullptr), &key, static_cast<uint32_t*>(nullptr)), 0u);
    CHECK_EQ(call<uint32_t>("advapi32.dll", "RegSetValueExW", key, u"V", 0u, 4u, reinterpret_cast<const uint8_t*>(&v), 4u), 0u);
    call<uint32_t>("advapi32.dll", "RegCloseKey", key);
    CHECK(fsys::exists(root / "overlay" / ".gta6rt-registry" / "hkcu" / "software" / "x"));
    CHECK_EQ(call<uint32_t>("kernel32.dll", "GetFileAttributesW", u"GAME:\\.gta6rt-registry"), 0xFFFFFFFFu);

    const char* args[] = {"-windowed", "a b"};
    CHECK_EQ(gta6rt_shims_set_command_line("G:\\game.exe", args, 2), GTA6RT_OK);
    CHECK(std::u16string(call<char16_t*>("kernel32.dll", "GetCommandLineW")) == u"G:\\game.exe -windowed \"a b\"");
    CHECK_EQ(gta6rt_shims_configure_paths(nullptr, "x"), GTA6RT_E_INVALID_ARG);

    gta6rt_shims_thread_detach();
    CHECK_EQ(call<int>("ucrtbase.dll", "_register_thread_local_exe_atexit_callback", reinterpret_cast<uint64_t>(&thread_exit_cb)), 0);
    gta6rt_shims_thread_detach();
    CHECK_EQ(g_thread_exit_args[1], 3u);
    CHECK_EQ(g_thread_exit_args[2], 0u);
    gta6rt_shims_configure_paths("", "");
    fsys::remove_all(root);
}
