// Test-only stand-in for shims_core's console path roots (rt::fs): the three functions the GDK shims call.
#include "file_system.h"

#include <windows.h>

#include <gta6rt/shims.h>

#include <mutex>
#include <string>

#include "gdk_text.h"

namespace rt::fs {
namespace {
std::mutex g_mu;
std::filesystem::path g_game, g_overlay;
}  // namespace

void configure(const std::filesystem::path& game_dir, const std::filesystem::path& overlay_dir) {
    std::lock_guard lock(g_mu);
    g_game = game_dir;
    g_overlay = overlay_dir;
}
std::filesystem::path game_dir() {
    std::lock_guard lock(g_mu);
    return g_game;
}
std::filesystem::path overlay_dir() {
    std::lock_guard lock(g_mu);
    return g_overlay;
}
void force_copy_file2_fallback(bool) {}

}  // namespace rt::fs

namespace {

// kernel32.dll!CreateFileW as shims_core's file_system serves it, reduced to the read side the DirectStorage shim
// uses: GAME:<rest> opens the overlay copy when there is one, else the game folder's; a path that climbs with ".." or
// names another drive is refused with ERROR_INVALID_NAME; anything else is a host path.
HANDLE WINAPI console_create_file_w(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                                    DWORD attributes, HANDLE tmpl) {
    std::string p = rt::gdk::to_utf8(path);
    if (p.size() >= 5 && (p.compare(0, 5, "GAME:") == 0 || p.compare(0, 5, "game:") == 0 || p.compare(0, 5, "Game:") == 0)) {
        std::string rest = p.substr(5);
        for (char& c : rest)
            if (c == '\\') c = '/';
        while (!rest.empty() && rest[0] == '/') rest.erase(0, 1);
        if (rest.find("..") != std::string::npos || (rest.size() >= 2 && rest[1] == ':')) {
            SetLastError(ERROR_INVALID_NAME);
            return INVALID_HANDLE_VALUE;
        }
        const auto overlay = rt::fs::overlay_dir() / rest;
        std::error_code ec;
        const auto chosen = std::filesystem::exists(overlay, ec) ? overlay : rt::fs::game_dir() / rest;
        p = chosen.string();
    }
    return ::CreateFileW(rt::gdk::to_u16(p).c_str(), access, share, sa, disposition, attributes, tmpl);
}

const gta6rt_shim_export kConsoleFiles[] = {
    {"CreateFileW", 0, GTA6RT_SHIM_CODE_NATIVE, reinterpret_cast<const void*>(&console_create_file_w)},
    {nullptr, 0, GTA6RT_SHIM_CODE_NATIVE, nullptr},
};

}  // namespace

void gdk_test_register_console_files() { gta6rt_shims_register_dll("kernel32.dll", kConsoleFiles); }
