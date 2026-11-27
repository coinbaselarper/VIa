// Console (GDK) file paths for a mapped title, on POSIX.
//
// A title's file API sees the console's drives: "GAME:\data\x.sps" is a file inside the installed game folder.
// Writes go to a writable overlay so the package stays exactly as installed:
//   GAME:<rest>    a read opens the overlay copy when one exists, else <game dir>/<rest>;
//                  an open that can write opens <overlay dir>/<rest>, copying the package file there first when the
//                  disposition keeps its contents; a delete leaves a marker in the overlay;
//   G:\ is the same folder, T:\ a scratch folder in the overlay.
// Any other path is a host path: "/abs" or a relative path, with '\' read as '/'. A Windows drive other than G: and T:
// names nothing on this host and fails with ERROR_PATH_NOT_FOUND. A GAME: path that climbs above the root with ".."
// is refused with ERROR_INVALID_NAME. Names compare case-insensitively inside GAME:/G:/T:, as the console's do.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace rt::fs {

// File handles created by gta6rt_os (GetStdHandle, os CreateFileW) share the FILE handle kind with this module's own.
// A handle this module did not create is forwarded to these os entry points; without them it fails with
// ERROR_INVALID_HANDLE. os_bindings.cpp installs them.
struct ForeignFileOps {
    BOOL(WINAPI* read_file)(HANDLE, void*, DWORD, DWORD*, void*);
    BOOL(WINAPI* write_file)(HANDLE, const void*, DWORD, DWORD*, void*);
    BOOL(WINAPI* get_file_size_ex)(HANDLE, int64_t*);
    BOOL(WINAPI* set_file_pointer_ex)(HANDLE, int64_t, int64_t*, DWORD);
    BOOL(WINAPI* set_end_of_file)(HANDLE);
    BOOL(WINAPI* flush_file_buffers)(HANDLE);
    BOOL(WINAPI* get_file_time)(HANDLE, FILETIME*, FILETIME*, FILETIME*);
    BOOL(WINAPI* set_file_time)(HANDLE, const FILETIME*, const FILETIME*, const FILETIME*);
    BOOL(WINAPI* get_file_information_by_handle)(HANDLE, BY_HANDLE_FILE_INFORMATION*);
    BOOL(WINAPI* read_file_ex)(HANDLE, void*, DWORD, void*, void*);
    BOOL(WINAPI* read_file_scatter)(HANDLE, void*, DWORD, DWORD*, void*);
    DWORD(WINAPI* queue_user_apc)(void*, HANDLE, ULONG_PTR);
    HANDLE(WINAPI* get_current_thread)();
};
void set_foreign_file_ops(const ForeignFileOps& ops);

inline constexpr char kPersistentStorageDir[] = ".gta6rt-persistent";

void configure(const std::string& game_dir_utf8, const std::string& overlay_dir_utf8);
std::string game_dir();
std::string overlay_dir();

struct HostPath {
    std::string path;
    uint32_t error = 0;  // non-zero: fail with this Win32 error and do not touch the host
};

// For the CRT's fopen/remove/_wstat64 on guest paths.
HostPath crt_read_path(const std::string& guest_utf8);
HostPath crt_write_path(const std::string& guest_utf8, bool keep_contents);
bool crt_remove(const std::string& guest_utf8, uint32_t& error);

}  // namespace rt::fs
