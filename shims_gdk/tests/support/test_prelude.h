#pragma once
// Force-included into the ported tests: Win32 helpers of the originals that compat does not declare, as test-local
// inline functions over POSIX, and the registration call.
#include <windows.h>
#include <objbase.h>
#include <propidl.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include <gta6rt/shims.h>

#include "gdk_register.h"

namespace gdk_test {
inline DWORD temp_path(DWORD size, char* out) {
    const char* dir = std::getenv("TMPDIR");
    std::string p = dir && *dir ? dir : "/tmp";
    if (p.back() != '/') p += '/';
    if (p.size() + 1 > size) return static_cast<DWORD>(p.size() + 1);
    std::snprintf(out, size, "%s", p.c_str());
    return static_cast<DWORD>(p.size());
}
inline DWORD temp_path_w(DWORD size, WCHAR* out) {
    char host[MAX_PATH];
    const DWORD n = temp_path(sizeof(host), host);
    if (n + 1 > size) return n + 1;
    for (DWORD i = 0; i <= n; ++i) out[i] = static_cast<unsigned char>(host[i]);
    return n;
}
inline UINT temp_file_name(const char* dir, const char* prefix, UINT, char* out) {
    std::string p = std::string(dir) + prefix + "XXXXXX";
    const int fd = ::mkstemp(p.data());
    if (fd < 0) return 0;
    ::close(fd);
    std::snprintf(out, MAX_PATH, "%s", p.c_str());
    return 1;
}
inline BOOL remove_directory(const char* path) { return ::rmdir(path) == 0; }

// Windows keeps a file open without share-delete from being replaced; on macOS the user-immutable flag refuses a
// rename over the file and a delete, which is what the tests use the open handle for.
inline bool hold_file(const std::string& path) { return ::chflags(path.c_str(), UF_IMMUTABLE) == 0; }
inline void release_file(const std::string& path) { ::chflags(path.c_str(), 0); }

inline HRESULT prop_variant_clear(PROPVARIANT* v) {
    if (v->vt == VT_LPWSTR && v->pwszVal) CoTaskMemFree(v->pwszVal);
    if (v->vt == VT_BLOB && v->blob.pBlobData) CoTaskMemFree(v->blob.pBlobData);
    PropVariantInit(v);
    return S_OK;
}
}  // namespace gdk_test

#define GetTempPathA gdk_test::temp_path
#define GetTempPathW gdk_test::temp_path_w
#define GetTempFileNameA gdk_test::temp_file_name
#define RemoveDirectoryA gdk_test::remove_directory
#define PropVariantClear gdk_test::prop_variant_clear

#if defined(__x86_64__)
#define GDK_TEST_REGISTER() gta6rt_shims_gdk_register()
#else
#define GDK_TEST_REGISTER() rt::gdk::register_all()
#endif
