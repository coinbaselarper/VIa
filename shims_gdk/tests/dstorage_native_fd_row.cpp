#include <windows.h>

#include <fcntl.h>
#include <gta6rt/shims.h>
#include <unistd.h>

#include <atomic>
#include <string>

std::atomic<int> g_native_opens{0};

static int WINAPI native_fd(HANDLE h) {
    WCHAR buf[2048];
    const DWORD n = GetFinalPathNameByHandleW(h, buf, 2048, 0);
    if (!n || n >= 2048) return -1;
    std::string p;
    for (DWORD i = 0; i < n; ++i) p.push_back(buf[i] < 128 ? static_cast<char>(buf[i]) : '?');
    if (p.rfind("\\\\?\\", 0) == 0) p = p.substr(4);
    if (p.size() > 2 && p[1] == ':') p = p.substr(2);
    for (char& c : p)
        if (c == '\\') c = '/';
    g_native_opens++;
    return open(p.c_str(), O_RDONLY | O_CLOEXEC);
}

#include <cstdio>
namespace {
struct Report {
    ~Report() { std::fprintf(stderr, "[vt] native fd opens=%d\n", g_native_opens.load()); }
} report;
struct Registrar {
    Registrar() {
        static const gta6rt_shim_export ex[] = {{"FileNativeFd", 0, GTA6RT_SHIM_CODE_NATIVE, reinterpret_cast<const void*>(&native_fd)},
                                                {nullptr, 0, GTA6RT_SHIM_CODE_NATIVE, nullptr}};
        gta6rt_shims_register_dll("gta6rt.internal", ex);
    }
} registrar;
}  // namespace
