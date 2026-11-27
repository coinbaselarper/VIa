// file_system: the file API of a mapped title, seen through the console's drives, on POSIX.
//
// A GDK title names its files on the console's drives ("GAME:\data\x.sps" is one in the installed game folder), so
// CreateFile*, ReadFile/WriteFile and their companions, FindFirstFile*/FindNextFile*/FindClose,
// GetFileAttributes*, DeleteFile*, CreateDirectory*, RemoveDirectory*, MoveFile*, CopyFile* and SetFileAttributes*
// are answered here. The rules (file_system.h has the summary):
//   - an open that can write, and DeleteFile, go to the overlay, so the package stays exactly as installed. A
//     write-open of a package file copies it into the overlay first, so OPEN_EXISTING keeps its bytes; a delete of a
//     package file leaves an empty marker under <overlay>/.gta6rt-deleted, and reads and listings treat the name as
//     gone. A re-created file is its overlay copy, which reads and listings prefer;
//   - reads prefer the overlay copy and then the package;
//   - FindFirstFile merges the two folders, the overlay winning on a name, and hides deleted names and the overlay's
//     own folders;
//   - a path that climbs above the root with ".." or names another drive is refused with ERROR_INVALID_NAME;
//   - names inside GAME:/G:/T: compare case-insensitively: a lookup that misses by exact spelling scans the folder.
// Win32 handles are the os handle table's: FILE for opened files and folders, FIND for enumerations.
#include <windows.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <gta6rt/os.h>

#include "file_system.h"
#include "k32_shared.h"
#include "shims.h"
#include "util.h"

namespace rt::fs {
namespace {

constexpr char kDeletedDir[] = ".gta6rt-deleted";
constexpr char kScratchDir[] = ".gta6rt-temp";
constexpr char kRemovedDirMarker[] = ".gta6rt-dir";
constexpr char kCopyDir[] = ".gta6rt-copy";
constexpr char kRegistryDir[] = ".gta6rt-registry";

constexpr DWORD kErrNoMoreFiles = 18, kErrFileExists = 80, kErrNotSameDevice = 17, kErrDirNotEmpty = 145,
                kErrAlreadyExists = 183, kErrDirectory = 267, kErrHandleEof = 38;

std::mutex g_mutex;
std::string g_game_dir;
std::string g_overlay_dir;

struct Roots {
    std::string game, overlay;
};

Roots roots() {
    std::lock_guard lock(g_mutex);
    return {g_game_dir, g_overlay_dir};
}

using Parts = std::vector<std::string>;

bool is_separator(char c) { return c == '\\' || c == '/'; }

bool same_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lower_ascii(std::string(1, a[i]))[0] != lower_ascii(std::string(1, b[i]))[0]) return false;
    return true;
}

bool internal_dir(const std::string& name) {
    return same_name(name, kDeletedDir) || same_name(name, kCopyDir) || same_name(name, kScratchDir) ||
           same_name(name, kRegistryDir) || same_name(name, kPersistentStorageDir);
}

DWORD win_error(int e) {
    switch (e) {
    case 0: return 0;
    case ENOENT: return 2;
    case ENOTDIR: return 3;
    case EACCES: case EPERM: case EROFS: return 5;
    case EEXIST: return kErrFileExists;
    case EISDIR: return 5;
    case ENOTEMPTY: return kErrDirNotEmpty;
    case EINVAL: return 87;
    case ENOMEM: return 8;
    case EMFILE: case ENFILE: return 4;
    case EXDEV: return kErrNotSameDevice;
    case ENOSPC: return 112;
    case ENAMETOOLONG: return 206;
    case EBUSY: return 32;
    default: return 31;
    }
}

// ---- case-insensitive name lookup, with a per-folder cache keyed by the folder's modification time

struct DirCache {
    int64_t mtime_ns = -1;
    std::unordered_map<std::string, std::string> by_lower;
};
std::mutex g_dir_cache_mutex;
std::map<std::string, DirCache> g_dir_cache;

int64_t mtime_ns(const struct stat& st) { return static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1000000000ll + st.st_mtimespec.tv_nsec; }

bool lookup_name_ci(const std::string& dir, const std::string& name, std::string& actual) {
    struct stat st;
    if (::stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    std::lock_guard lock(g_dir_cache_mutex);
    DirCache& cache = g_dir_cache[dir];
    if (cache.mtime_ns != mtime_ns(st)) {
        cache.by_lower.clear();
        if (DIR* d = ::opendir(dir.c_str())) {
            while (dirent* e = ::readdir(d)) cache.by_lower.emplace(lower_ascii(e->d_name), e->d_name);
            ::closedir(d);
        }
        cache.mtime_ns = mtime_ns(st);
    }
    auto it = cache.by_lower.find(lower_ascii(name));
    if (it == cache.by_lower.end()) return false;
    actual = it->second;
    return true;
}

void forget_dir_cache(const std::string& dir) {
    std::lock_guard lock(g_dir_cache_mutex);
    g_dir_cache.erase(dir);
}

struct Resolved {
    std::string path;      // the host path, case-corrected as far as it exists
    bool exists = false;   // the whole path exists
    bool parent_exists = false;
};

Resolved resolve(const std::string& root, const Parts& parts) {
    Resolved r;
    r.path = root;
    r.parent_exists = true;
    r.exists = true;
    struct stat st;
    for (size_t i = 0; i < parts.size(); ++i) {
        const bool last = i + 1 == parts.size();
        std::string cand = r.path + "/" + parts[i];
        std::string actual;
        if (::stat(cand.c_str(), &st) == 0) {
            r.path = cand;
        } else if (lookup_name_ci(r.path, parts[i], actual)) {
            r.path += "/" + actual;
        } else {
            r.path = cand;
            r.exists = false;
            r.parent_exists = last;
            for (size_t j = i + 1; j < parts.size(); ++j) r.path += "/" + parts[j];
            return r;
        }
    }
    return r;
}

std::string join(const std::string& root, const Parts& parts) {
    std::string p = root;
    for (const auto& part : parts) p += "/" + part;
    return p;
}

// ---- guest path parsing

enum class Parsed { Host, Console, Refused, Invalid };
enum class Drive { None, Game, Scratch };

struct GuestPath {
    Parsed kind = Parsed::Host;
    Drive drive = Drive::None;
    Parts parts;
    std::string host;
};

GuestPath parse(const std::string& guest) {
    GuestPath out;
    std::string path = guest;
    if (path.rfind("\\\\?\\", 0) == 0 || path.rfind("\\\\.\\", 0) == 0) path.erase(0, 4);
    size_t prefix = 0;
    Drive drive = Drive::None;
    if (path.size() >= 5 && lower_ascii(path.substr(0, 5)) == "game:") {
        drive = Drive::Game;
        prefix = 5;
    } else if (path.size() >= 2 && path[1] == ':' && std::isalpha(static_cast<unsigned char>(path[0]))) {
        const char letter = lower_ascii(std::string(1, path[0]))[0];
        const Roots r = roots();
        if ((letter == 'g' || letter == 't') && !(r.game.empty() && r.overlay.empty()) &&
            (path.size() == 2 || is_separator(path[2]))) {
            drive = letter == 'g' ? Drive::Game : Drive::Scratch;
            prefix = 2;
        } else {
            out.kind = Parsed::Invalid;
            return out;
        }
    }
    if (drive == Drive::None) {
        std::string host = path;
        for (auto& c : host)
            if (c == '\\') c = '/';
        out.host = host;
        return out;
    }
    out.kind = Parsed::Console;
    out.drive = drive;
    if (drive == Drive::Scratch) out.parts.push_back(kScratchDir);
    const size_t floor = out.parts.size();
    std::string part;
    auto flush = [&]() -> bool {
        if (part.empty() || part == ".") {
            part.clear();
            return true;
        }
        if (part == "..") {
            if (out.parts.size() <= floor) return false;
            out.parts.pop_back();
        } else if (part.find(':') != std::string::npos) {
            return false;
        } else {
            out.parts.push_back(part);
        }
        part.clear();
        return true;
    };
    for (size_t i = prefix; i < path.size(); ++i) {
        if (is_separator(path[i])) {
            if (!flush()) {
                out.kind = Parsed::Refused;
                return out;
            }
        } else {
            part.push_back(path[i]);
        }
    }
    if (!flush() || (drive == Drive::Game && !out.parts.empty() && internal_dir(out.parts.front()))) {
        out.kind = Parsed::Refused;
        return out;
    }
    if (drive == Drive::Scratch) {
        const Roots r = roots();
        if (!r.overlay.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(join(r.overlay, {kScratchDir}), ec);
        }
    }
    return out;
}

bool drive_root(const GuestPath& p) { return p.parts.empty() || (p.drive == Drive::Scratch && p.parts.size() == 1); }

// ---- the overlay's deletion markers

std::string marker(const std::string& overlay, const Parts& parts) {
    Parts lowered;
    for (const auto& p : parts) lowered.push_back(lower_ascii(p));
    return join(overlay + "/" + kDeletedDir, lowered);
}

bool deleted(const std::string& overlay, const Parts& parts) {
    if (overlay.empty() || parts.empty()) return false;
    struct stat st;
    const std::string m = marker(overlay, parts);
    if (::stat(m.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return true;
    const std::string d = m + "/" + kRemovedDirMarker;
    return ::stat(d.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool write_marker(const std::string& path, DWORD& error) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    if (ec) {
        error = win_error(ec.value());
        return false;
    }
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        error = win_error(errno);
        return false;
    }
    ::close(fd);
    return true;
}

enum class Kind { None, File, Directory };
struct View {
    Kind kind = Kind::None;
    bool in_overlay = false;
    bool in_package = false;
    std::string overlay_path, package_path;
};

Kind kind_of(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return Kind::None;
    return S_ISDIR(st.st_mode) ? Kind::Directory : Kind::File;
}

View view(const Roots& r, const Parts& parts) {
    View v;
    if (parts.empty()) {
        v.kind = Kind::Directory;
        v.in_package = !r.game.empty();
        v.package_path = r.game;
        return v;
    }
    if (!r.overlay.empty()) {
        const Resolved res = resolve(r.overlay, parts);
        if (res.exists) {
            v.in_overlay = true;
            v.overlay_path = res.path;
            v.kind = kind_of(res.path);
        }
    }
    if (!r.game.empty() && !deleted(r.overlay, parts)) {
        const Resolved res = resolve(r.game, parts);
        if (res.exists) {
            v.in_package = true;
            v.package_path = res.path;
            if (v.kind == Kind::None) v.kind = kind_of(res.path);
        }
    }
    return v;
}

bool is_folder(const Roots& r, const Parts& parts) { return view(r, parts).kind == Kind::Directory; }

Parts parent_of(const Parts& parts) { return parts.empty() ? parts : Parts(parts.begin(), parts.end() - 1); }

DWORD missing_error(const Roots& r, const Parts& parts) { return is_folder(r, parent_of(parts)) ? 2 : 3; }

bool empty_in_view(const Roots& r, const Parts& parts) {
    auto scan = [&](const std::string& root, bool overlay_root) {
        const Resolved res = resolve(root, parts);
        if (!res.exists) return true;
        DIR* d = ::opendir(res.path.c_str());
        if (!d) return true;
        bool empty = true;
        while (dirent* e = ::readdir(d)) {
            const std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            if (overlay_root) {
                if (parts.empty() && internal_dir(name)) continue;
            } else {
                Parts child = parts;
                child.push_back(name);
                if (deleted(r.overlay, child)) continue;
            }
            empty = false;
            break;
        }
        ::closedir(d);
        return empty;
    };
    if (!r.overlay.empty() && !scan(r.overlay, true)) return false;
    if (!r.game.empty() && !scan(r.game, false)) return false;
    return true;
}

struct Target {
    std::string path;
    DWORD error = 0;
};

Target fail(DWORD error) {
    Target t;
    t.error = error;
    return t;
}

Target host_target(const GuestPath& p) {
    Target t;
    t.path = p.host;
    if (t.path.empty()) t.error = 3;
    return t;
}

Target read_target(const std::string& guest) {
    const GuestPath p = parse(guest);
    if (p.kind == Parsed::Invalid) return fail(3);
    if (p.kind == Parsed::Host) return host_target(p);
    if (p.kind == Parsed::Refused) return fail(123);
    const Roots r = roots();
    if (r.game.empty()) return fail(3);
    if (!r.overlay.empty()) {
        const Resolved res = resolve(r.overlay, p.parts);
        if (res.exists) {
            Target t;
            t.path = res.path;
            return t;
        }
    }
    if (deleted(r.overlay, p.parts)) return fail(2);
    Target t;
    t.path = resolve(r.game, p.parts).path;
    return t;
}

std::mutex g_copy_mutex;
unsigned g_copy_count = 0;

bool copy_file_contents(const std::string& from, const std::string& to, bool overwrite, DWORD& error) {
    std::error_code ec;
    std::filesystem::copy_file(from, to,
                               overwrite ? std::filesystem::copy_options::overwrite_existing : std::filesystem::copy_options::none, ec);
    if (ec) {
        error = win_error(ec.value());
        return false;
    }
    return true;
}

Target write_target(const std::string& guest, bool copy_up) {
    const GuestPath p = parse(guest);
    if (p.kind == Parsed::Invalid) return fail(3);
    if (p.kind == Parsed::Host) return host_target(p);
    if (p.kind == Parsed::Refused) return fail(123);
    const Roots r = roots();
    if (r.overlay.empty()) return fail(3);
    if (p.parts.empty()) return fail(123);
    const bool scratch = same_name(p.parts.front(), kScratchDir);
    if (!scratch && !is_folder(r, parent_of(p.parts))) return fail(3);

    const Resolved res = resolve(r.overlay, p.parts);
    Target t;
    t.path = res.path;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(t.path).parent_path(), ec);
    if (ec) {
        t.error = win_error(ec.value());
        return t;
    }
    forget_dir_cache(std::filesystem::path(t.path).parent_path().string());

    std::unique_lock copy_lock(g_copy_mutex, std::defer_lock);
    if (copy_up) copy_lock.lock();
    if (copy_up && !deleted(r.overlay, p.parts) && !res.exists && !r.game.empty()) {
        const Resolved pkg = resolve(r.game, p.parts);
        struct stat st;
        if (pkg.exists && ::stat(pkg.path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            const std::string staging = r.overlay + "/" + kCopyDir;
            std::filesystem::create_directories(staging, ec);
            const std::string tmp = staging + "/" + std::to_string(::getpid()) + "-" + std::to_string(++g_copy_count);
            DWORD error = 0;
            if (ec || !copy_file_contents(pkg.path, tmp, true, error)) {
                t.error = ec ? win_error(ec.value()) : error;
                std::filesystem::remove(tmp, ec);
                return t;
            }
            if (::rename(tmp.c_str(), t.path.c_str()) != 0) {
                const int e = errno;
                std::filesystem::remove(tmp, ec);
                if (::stat(t.path.c_str(), &st) != 0) t.error = win_error(e);
            }
        }
    }
    return t;
}

constexpr DWORD kGenericRead = 0x80000000u, kGenericWrite = 0x40000000u, kGenericAll = 0x10000000u,
                kMaximumAllowed = 0x02000000u, kFileWriteData = 2, kFileAppendData = 4, kFileWriteEa = 0x10,
                kFileWriteAttributes = 0x100, kDelete = 0x10000, kWriteDac = 0x40000, kWriteOwner = 0x80000;

bool writes(DWORD access, DWORD disposition) {
    constexpr DWORD kWriteAccess = kGenericWrite | kGenericAll | kMaximumAllowed | kFileWriteData | kFileAppendData |
                                   kFileWriteEa | kFileWriteAttributes | kDelete | kWriteDac | kWriteOwner;
    if ((access & kWriteAccess) != 0) return true;
    return disposition == CREATE_NEW || disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS ||
           disposition == TRUNCATE_EXISTING;
}

Target open_target(const std::string& guest, DWORD access, DWORD disposition) {
    if (!writes(access, disposition)) return read_target(guest);
    const bool keeps = disposition == OPEN_EXISTING || disposition == OPEN_ALWAYS || disposition == TRUNCATE_EXISTING;
    return write_target(guest, keeps);
}

// ---- file objects

struct FileObj {
    int fd = -1;
    bool is_dir = false;
    bool can_read = true, can_write = false, overlapped = false, append = false, delete_on_close = false;
    std::mutex m;
    uint64_t pos = 0;
    std::string path;
};

std::mutex g_files_mutex;
std::unordered_set<const void*> g_files;
ForeignFileOps g_foreign{};

void close_file(void* p) {
    auto* f = static_cast<FileObj*>(p);
    {
        std::lock_guard lock(g_files_mutex);
        g_files.erase(f);
    }
    if (f->fd >= 0) ::close(f->fd);
    if (f->delete_on_close) ::unlink(f->path.c_str());
    delete f;
}

HANDLE publish(FileObj* f) {
    uint64_t h = 0;
    {
        std::lock_guard lock(g_files_mutex);
        g_files.insert(f);
    }
    if (gta6rt_handle_alloc(GTA6RT_HANDLE_FILE, f, &close_file, &h) != GTA6RT_OK) {
        close_file(f);
        gta6rt_last_error_set(8);
        return INVALID_HANDLE_VALUE;
    }
    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(h));
}

struct FileRef {
    FileObj* core = nullptr;
    bool foreign = false;
};

FileRef file_ref(HANDLE h) {
    void* o = gta6rt_handle_lookup(reinterpret_cast<uintptr_t>(h), GTA6RT_HANDLE_FILE);
    if (!o) return {};
    {
        std::lock_guard lock(g_files_mutex);
        if (g_files.count(o)) return {static_cast<FileObj*>(o), false};
    }
    return {nullptr, true};
}

int WINAPI file_native_fd(HANDLE h) {
    const FileRef rf = file_ref(h);
    return rf.core && !rf.core->is_dir ? rf.core->fd : -1;
}

BOOL foreign_unavailable() {
    gta6rt_last_error_set(6);
    return FALSE;
}

HANDLE open_host_file(const std::string& path, DWORD access, DWORD disposition, DWORD flags) {
    struct stat st;
    const bool exists = ::stat(path.c_str(), &st) == 0;
    const bool is_dir = exists && S_ISDIR(st.st_mode);
    const bool wants_write = writes(access, disposition);

    if (disposition == CREATE_NEW && exists) {
        gta6rt_last_error_set(kErrFileExists);
        return INVALID_HANDLE_VALUE;
    }
    if (!exists && (disposition == OPEN_EXISTING || disposition == TRUNCATE_EXISTING)) {
        std::string parent = std::filesystem::path(path).parent_path().string();
        if (parent.empty()) parent = ".";
        struct stat pst;
        gta6rt_last_error_set(::stat(parent.c_str(), &pst) == 0 ? 2 : 3);
        return INVALID_HANDLE_VALUE;
    }
    if (is_dir) {
        if (!(flags & 0x02000000u)) {
            gta6rt_last_error_set(5);
            return INVALID_HANDLE_VALUE;
        }
        auto* f = new FileObj;
        f->is_dir = true;
        f->path = path;
        f->fd = ::open(path.c_str(), O_RDONLY);
        if (disposition == CREATE_NEW || disposition == CREATE_ALWAYS || disposition == TRUNCATE_EXISTING) {
            close_file(f);
            gta6rt_last_error_set(5);
            return INVALID_HANDLE_VALUE;
        }
        return publish(f);
    }
    if (exists && !S_ISREG(st.st_mode) && !S_ISCHR(st.st_mode)) {
        gta6rt_last_error_set(5);
        return INVALID_HANDLE_VALUE;
    }

    int of = wants_write ? O_RDWR : O_RDONLY;
    if (wants_write && !(access & (kGenericRead | kGenericAll | kMaximumAllowed | 1))) of = O_WRONLY;
    if (disposition == CREATE_NEW) of |= O_CREAT | O_EXCL;
    else if (disposition == CREATE_ALWAYS) of |= O_CREAT | O_TRUNC;
    else if (disposition == OPEN_ALWAYS) of |= O_CREAT;
    else if (disposition == TRUNCATE_EXISTING) of |= O_TRUNC;
    int fd = ::open(path.c_str(), of, 0644);
    if (fd < 0 && wants_write && (of & O_ACCMODE) == O_RDWR && errno == EACCES) {
        of = (of & ~O_ACCMODE) | O_WRONLY;
        fd = ::open(path.c_str(), of, 0644);
    }
    if (fd < 0) {
        gta6rt_last_error_set(win_error(errno));
        return INVALID_HANDLE_VALUE;
    }
    auto* f = new FileObj;
    f->fd = fd;
    f->path = path;
    f->can_write = wants_write;
    f->overlapped = (flags & FILE_FLAG_OVERLAPPED) != 0;
    f->delete_on_close = (flags & 0x04000000u) != 0;
    HANDLE h = publish(f);
    if (h != INVALID_HANDLE_VALUE)
        gta6rt_last_error_set((exists && (disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS)) ? kErrAlreadyExists : 0);
    return h;
}

HANDLE WINAPI create_file_w(const char16_t* name, DWORD access, DWORD share, void* attributes, DWORD disposition,
                            DWORD flags, HANDLE template_file) {
    (void)share; (void)attributes; (void)template_file;
    if (!name || disposition < 1 || disposition > 5) {
        gta6rt_last_error_set(87);
        return INVALID_HANDLE_VALUE;
    }
    const std::string guest = narrow(name);
    const Target t = open_target(guest, access, disposition);
    if (t.error) {
        gta6rt_log(GTA6RT_LOG_DEBUG, "fs", "CreateFileW(%s) failed: %u", guest.c_str(), t.error);
        gta6rt_last_error_set(t.error);
        return INVALID_HANDLE_VALUE;
    }
    HANDLE h = open_host_file(t.path, access, disposition, flags);
    if (h == INVALID_HANDLE_VALUE)
        gta6rt_log(GTA6RT_LOG_DEBUG, "fs", "CreateFileW(%s) failed: %u", guest.c_str(), gta6rt_last_error_get());
    return h;
}

HANDLE WINAPI create_file_a(const char* name, DWORD access, DWORD share, void* attributes, DWORD disposition, DWORD flags,
                            HANDLE template_file) {
    if (!name) {
        gta6rt_last_error_set(87);
        return INVALID_HANDLE_VALUE;
    }
    const std::u16string wide = widen(name);
    return create_file_w(wide.c_str(), access, share, attributes, disposition, flags, template_file);
}

HANDLE WINAPI create_file2(const char16_t* name, DWORD access, DWORD share, DWORD disposition,
                           CREATEFILE2_EXTENDED_PARAMETERS* params) {
    if (!name || (params && params->dwSize < sizeof *params)) {
        gta6rt_last_error_set(87);
        return INVALID_HANDLE_VALUE;
    }
    const DWORD flags = params ? (params->dwFileAttributes | params->dwFileFlags) : 0;
    return create_file_w(name, access, share, params ? params->lpSecurityAttributes : nullptr, disposition, flags, nullptr);
}

// ---- reads and writes

struct OverlappedLayout {
    ULONG_PTR Internal, InternalHigh;
    uint32_t Offset, OffsetHigh;
    HANDLE hEvent;
};

void complete_overlapped(OverlappedLayout* ov, uint64_t bytes) {
    ov->Internal = 0;
    ov->InternalHigh = bytes;
    if (ov->hEvent) {
        const auto key = reinterpret_cast<uintptr_t>(ov->hEvent) & ~uintptr_t{1};
        if (auto* w = static_cast<gta6rt_waitable*>(gta6rt_handle_lookup(key, GTA6RT_HANDLE_WAITABLE))) gta6rt_event_set(w);
    }
}

BOOL WINAPI read_file(HANDLE h, void* buffer, DWORD bytes, DWORD* read, OverlappedLayout* ov) {
    const FileRef rf = file_ref(h);
    if (rf.foreign) {
        if (g_foreign.read_file) return g_foreign.read_file(h, buffer, bytes, read, ov);
        if (read) *read = 0;
        return foreign_unavailable();
    }
    FileObj* f = rf.core;
    if (read) *read = 0;
    if (!f || f->is_dir) {
        gta6rt_last_error_set(f ? 5 : 6);
        return FALSE;
    }
    if (!buffer && bytes) {
        gta6rt_last_error_set(87);
        return FALSE;
    }
    uint64_t offset;
    std::unique_lock lock(f->m, std::defer_lock);
    if (ov) {
        offset = (static_cast<uint64_t>(ov->OffsetHigh) << 32) | ov->Offset;
    } else {
        lock.lock();
        offset = f->pos;
    }
    size_t total = 0;
    while (total < bytes) {
        const ssize_t n = ::pread(f->fd, static_cast<char*>(buffer) + total, bytes - total, static_cast<off_t>(offset + total));
        if (n < 0) {
            if (errno == EINTR) continue;
            gta6rt_last_error_set(win_error(errno));
            return FALSE;
        }
        if (n == 0) break;
        total += static_cast<size_t>(n);
    }
    if (!ov) f->pos = offset + total;
    if (read) *read = static_cast<DWORD>(total);
    if (ov) {
        if (total == 0 && bytes > 0) {
            ov->Internal = 0xC0000011u;
            ov->InternalHigh = 0;
            gta6rt_last_error_set(kErrHandleEof);
            return FALSE;
        }
        complete_overlapped(ov, total);
    }
    gta6rt_last_error_set(0);
    return TRUE;
}

BOOL WINAPI write_file(HANDLE h, const void* buffer, DWORD bytes, DWORD* written, OverlappedLayout* ov) {
    const FileRef rf = file_ref(h);
    if (rf.foreign) {
        if (g_foreign.write_file) return g_foreign.write_file(h, buffer, bytes, written, ov);
        if (written) *written = 0;
        return foreign_unavailable();
    }
    FileObj* f = rf.core;
    if (written) *written = 0;
    if (!f || f->is_dir) {
        gta6rt_last_error_set(f ? 5 : 6);
        return FALSE;
    }
    if (!f->can_write) {
        gta6rt_last_error_set(5);
        return FALSE;
    }
    uint64_t offset;
    std::unique_lock lock(f->m, std::defer_lock);
    if (ov) {
        offset = (static_cast<uint64_t>(ov->OffsetHigh) << 32) | ov->Offset;
    } else {
        lock.lock();
        offset = f->pos;
    }
    size_t total = 0;
    while (total < bytes) {
        const ssize_t n = ::pwrite(f->fd, static_cast<const char*>(buffer) + total, bytes - total, static_cast<off_t>(offset + total));
        if (n < 0) {
            if (errno == EINTR) continue;
            gta6rt_last_error_set(win_error(errno));
            return FALSE;
        }
        total += static_cast<size_t>(n);
    }
    if (!ov) f->pos = offset + total;
    if (written) *written = static_cast<DWORD>(total);
    if (ov) complete_overlapped(ov, total);
    gta6rt_last_error_set(0);
    return TRUE;
}

struct ReadExCompletion {
    void* routine;
    DWORD error, bytes;
    OverlappedLayout* ov;
};

void WINAPI read_file_ex_apc(ULONG_PTR data) {
    std::unique_ptr<ReadExCompletion> c(reinterpret_cast<ReadExCompletion*>(data));
    const uint64_t args[3] = {c->error, c->bytes, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(c->ov))};
    gta6rt_os_invoke(static_cast<gta6rt_gva>(reinterpret_cast<uintptr_t>(c->routine)), args, 3);
}

BOOL WINAPI read_file_ex(HANDLE h, void* buffer, DWORD bytes, OverlappedLayout* ov, void* routine) {
    const FileRef rf = file_ref(h);
    if (rf.foreign)
        return g_foreign.read_file_ex ? g_foreign.read_file_ex(h, buffer, bytes, ov, routine) : foreign_unavailable();
    FileObj* f = rf.core;
    if (!f) {
        gta6rt_last_error_set(6);
        return FALSE;
    }
    if (!ov || !routine || (!buffer && bytes)) {
        gta6rt_last_error_set(87);
        return FALSE;
    }
    if (f->is_dir) {
        gta6rt_last_error_set(5);
        return FALSE;
    }
    if (!g_foreign.queue_user_apc || !g_foreign.get_current_thread) {
        gta6rt_log(GTA6RT_LOG_WARN, "fs", "ReadFileEx: no APC queue available, failing with ERROR_NOT_SUPPORTED");
        gta6rt_last_error_set(50);
        return FALSE;
    }
    const uint64_t offset = (static_cast<uint64_t>(ov->OffsetHigh) << 32) | ov->Offset;
    size_t total = 0;
    DWORD error = 0;
    while (total < bytes) {
        const ssize_t n = ::pread(f->fd, static_cast<char*>(buffer) + total, bytes - total, static_cast<off_t>(offset + total));
        if (n < 0) {
            if (errno == EINTR) continue;
            error = win_error(errno);
            break;
        }
        if (n == 0) break;
        total += static_cast<size_t>(n);
    }
    if (!error && total == 0 && bytes > 0) error = kErrHandleEof;
    ov->Internal = error == kErrHandleEof ? 0xC0000011u : (error ? 0xC0000001u : 0);
    ov->InternalHigh = total;
    auto* c = new ReadExCompletion{routine, error, static_cast<DWORD>(total), ov};
    if (!g_foreign.queue_user_apc(reinterpret_cast<void*>(&read_file_ex_apc), g_foreign.get_current_thread(),
                                  reinterpret_cast<ULONG_PTR>(c))) {
        delete c;
        return FALSE;
    }
    gta6rt_last_error_set(0);
    return TRUE;
}

BOOL WINAPI read_file_scatter(HANDLE h, FILE_SEGMENT_ELEMENT* segments, DWORD bytes, DWORD* reserved, OverlappedLayout* ov) {
    const FileRef rf = file_ref(h);
    if (rf.foreign)
        return g_foreign.read_file_scatter ? g_foreign.read_file_scatter(h, segments, bytes, reserved, ov) : foreign_unavailable();
    FileObj* f = rf.core;
    if (!f) {
        gta6rt_last_error_set(6);
        return FALSE;
    }
    constexpr DWORD kPage = 4096;
    if (!ov || reserved || !segments || bytes % kPage != 0 || !f->overlapped) {
        gta6rt_last_error_set(87);
        return FALSE;
    }
    if (f->is_dir) {
        gta6rt_last_error_set(5);
        return FALSE;
    }
    const uint64_t offset = (static_cast<uint64_t>(ov->OffsetHigh) << 32) | ov->Offset;
    size_t total = 0;
    bool eof = false;
    for (DWORD i = 0; i < bytes / kPage && !eof; ++i) {
        auto* dst = reinterpret_cast<char*>(static_cast<uintptr_t>(segments[i].Alignment));
        if (!dst) {
            gta6rt_last_error_set(87);
            return FALSE;
        }
        size_t got = 0;
        while (got < kPage) {
            const ssize_t n = ::pread(f->fd, dst + got, kPage - got, static_cast<off_t>(offset + total + got));
            if (n < 0) {
                if (errno == EINTR) continue;
                gta6rt_last_error_set(win_error(errno));
                return FALSE;
            }
            if (n == 0) {
                eof = true;
                break;
            }
            got += static_cast<size_t>(n);
        }
        total += got;
    }
    if (total == 0 && bytes > 0) {
        ov->Internal = 0xC0000011u;
        ov->InternalHigh = 0;
        gta6rt_last_error_set(kErrHandleEof);
        return FALSE;
    }
    complete_overlapped(ov, total);
    gta6rt_last_error_set(0);
    return TRUE;
}

BOOL WINAPI get_file_size_ex(HANDLE h, int64_t* size) {
    const FileRef rf = file_ref(h);
    if (rf.foreign) return g_foreign.get_file_size_ex ? g_foreign.get_file_size_ex(h, size) : foreign_unavailable();
    FileObj* f = rf.core;
    struct stat st;
    if (!f || !size || f->fd < 0 || ::fstat(f->fd, &st) != 0) {
        gta6rt_last_error_set(f ? 87 : 6);
        return FALSE;
    }
    *size = st.st_size;
    return TRUE;
}

DWORD WINAPI get_file_size(HANDLE h, DWORD* high) {
    int64_t size;
    if (!get_file_size_ex(h, &size)) return 0xFFFFFFFFu;
    if (high) *high = static_cast<DWORD>(static_cast<uint64_t>(size) >> 32);
    gta6rt_last_error_set(0);
    return static_cast<DWORD>(size);
}

BOOL WINAPI set_file_pointer_ex(HANDLE h, int64_t distance, int64_t* new_pos, DWORD method) {
    const FileRef rf = file_ref(h);
    if (rf.foreign) return g_foreign.set_file_pointer_ex ? g_foreign.set_file_pointer_ex(h, distance, new_pos, method) : foreign_unavailable();
    FileObj* f = rf.core;
    if (!f) {
        gta6rt_last_error_set(6);
        return FALSE;
    }
    std::lock_guard lock(f->m);
    int64_t base;
    switch (method) {
    case 0: base = 0; break;
    case 1: base = static_cast<int64_t>(f->pos); break;
    case 2: {
        struct stat st;
        if (f->fd < 0 || ::fstat(f->fd, &st) != 0) {
            gta6rt_last_error_set(87);
            return FALSE;
        }
        base = st.st_size;
        break;
    }
    default: gta6rt_last_error_set(87); return FALSE;
    }
    const int64_t target = base + distance;
    if (target < 0) {
        gta6rt_last_error_set(131);
        return FALSE;
    }
    f->pos = static_cast<uint64_t>(target);
    if (new_pos) *new_pos = target;
    return TRUE;
}

DWORD WINAPI set_file_pointer(HANDLE h, int32_t low, int32_t* high, DWORD method) {
    int64_t distance = low;
    if (high) distance = (static_cast<int64_t>(*high) << 32) | static_cast<uint32_t>(low);
    int64_t result;
    if (!set_file_pointer_ex(h, distance, &result, method)) return 0xFFFFFFFFu;
    if (high) *high = static_cast<int32_t>(result >> 32);
    gta6rt_last_error_set(0);
    return static_cast<DWORD>(result);
}

BOOL WINAPI set_end_of_file(HANDLE h) {
    const FileRef rf = file_ref(h);
    if (rf.foreign) return g_foreign.set_end_of_file ? g_foreign.set_end_of_file(h) : foreign_unavailable();
    FileObj* f = rf.core;
    if (!f || f->fd < 0) {
        gta6rt_last_error_set(f ? 87 : 6);
        return FALSE;
    }
    if (!f->can_write) {
        gta6rt_last_error_set(5);
        return FALSE;
    }
    std::lock_guard lock(f->m);
    if (::ftruncate(f->fd, static_cast<off_t>(f->pos)) != 0) {
        gta6rt_last_error_set(win_error(errno));
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI flush_file_buffers(HANDLE h) {
    const FileRef rf = file_ref(h);
    if (rf.foreign) return g_foreign.flush_file_buffers ? g_foreign.flush_file_buffers(h) : foreign_unavailable();
    FileObj* f = rf.core;
    if (!f || f->fd < 0) {
        gta6rt_last_error_set(f ? 5 : 6);
        return FALSE;
    }
    if (!f->can_write) {
        gta6rt_last_error_set(5);
        return FALSE;
    }
    if (::fsync(f->fd) != 0) {
        gta6rt_last_error_set(win_error(errno));
        return FALSE;
    }
    return TRUE;
}

void set_ft(const struct timespec& ts, FILETIME* ft) {
    const uint64_t v = k32::filetime_from_unix(ts.tv_sec, ts.tv_nsec);
    ft->dwLowDateTime = static_cast<DWORD>(v);
    ft->dwHighDateTime = static_cast<DWORD>(v >> 32);
}

BOOL WINAPI get_file_time(HANDLE h, FILETIME* creation, FILETIME* access, FILETIME* write) {
    const FileRef rf = file_ref(h);
    if (rf.foreign) return g_foreign.get_file_time ? g_foreign.get_file_time(h, creation, access, write) : foreign_unavailable();
    FileObj* f = rf.core;
    struct stat st;
    if (!f || f->fd < 0 || ::fstat(f->fd, &st) != 0) {
        gta6rt_last_error_set(f ? 87 : 6);
        return FALSE;
    }
    if (creation) set_ft(st.st_birthtimespec, creation);
    if (access) set_ft(st.st_atimespec, access);
    if (write) set_ft(st.st_mtimespec, write);
    return TRUE;
}

BOOL WINAPI set_file_time(HANDLE h, const FILETIME* creation, const FILETIME* access, const FILETIME* write) {
    const FileRef rf = file_ref(h);
    if (rf.foreign)
        return g_foreign.set_file_time ? g_foreign.set_file_time(h, creation, access, write) : foreign_unavailable();
    FileObj* f = rf.core;
    if (!f || f->fd < 0) {
        gta6rt_last_error_set(f ? 87 : 6);
        return FALSE;
    }
    struct timespec times[2] = {{0, UTIME_OMIT}, {0, UTIME_OMIT}};
    auto conv = [](const FILETIME* ft, struct timespec* ts) {
        const uint64_t v = (static_cast<uint64_t>(ft->dwHighDateTime) << 32) | ft->dwLowDateTime;
        if (v == 0 || v == 0xFFFFFFFFFFFFFFFFull) return;
        const uint64_t unix100 = v - 116444736000000000ull;
        ts->tv_sec = static_cast<time_t>(unix100 / 10000000ull);
        ts->tv_nsec = static_cast<long>((unix100 % 10000000ull) * 100);
    };
    if (access) conv(access, &times[0]);
    if (write) conv(write, &times[1]);
    if (::futimens(f->fd, times) != 0) {
        gta6rt_last_error_set(win_error(errno));
        return FALSE;
    }
    return TRUE;
}

// ---- attributes and directory listings

DWORD attributes_of(const struct stat& st) {
    DWORD a = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    if (!(st.st_mode & S_IWUSR)) a |= FILE_ATTRIBUTE_READONLY;
    return a;
}

void fill_times(const struct stat& st, FILETIME* c, FILETIME* a, FILETIME* w) {
    set_ft(st.st_birthtimespec, c);
    set_ft(st.st_atimespec, a);
    set_ft(st.st_mtimespec, w);
}

void fill_find(const std::string& name, const struct stat& st, WIN32_FIND_DATAW* d) {
    std::memset(d, 0, sizeof *d);
    d->dwFileAttributes = attributes_of(st);
    fill_times(st, &d->ftCreationTime, &d->ftLastAccessTime, &d->ftLastWriteTime);
    const uint64_t size = S_ISDIR(st.st_mode) ? 0 : static_cast<uint64_t>(st.st_size);
    d->nFileSizeHigh = static_cast<DWORD>(size >> 32);
    d->nFileSizeLow = static_cast<DWORD>(size);
    const std::u16string wide = widen(name.c_str());
    const size_t n = std::min<size_t>(wide.size(), 259);
    std::memcpy(d->cFileName, wide.data(), n * sizeof(char16_t));
}

bool wildcard_match(const std::string& pattern, const std::string& name) {
    const std::string p = lower_ascii(pattern == "*.*" ? std::string("*") : pattern);
    const std::string n = lower_ascii(name);
    size_t pi = 0, ni = 0, star = std::string::npos, mark = 0;
    while (ni < n.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == n[ni])) {
            ++pi;
            ++ni;
        } else if (pi < p.size() && p[pi] == '*') {
            star = pi++;
            mark = ni;
        } else if (star != std::string::npos) {
            pi = star + 1;
            ni = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

struct Enumeration {
    std::vector<WIN32_FIND_DATAW> entries;
    size_t next = 0;
};

void close_enumeration(void* p) { delete static_cast<Enumeration*>(p); }

HANDLE publish_enumeration(std::unique_ptr<Enumeration> e) {
    uint64_t h = 0;
    if (gta6rt_handle_alloc(GTA6RT_HANDLE_FIND, e.get(), &close_enumeration, &h) != GTA6RT_OK) {
        gta6rt_last_error_set(8);
        return INVALID_HANDLE_VALUE;
    }
    e.release();
    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(h));
}

void list_dir(const std::string& root, const Parts& dirs, const std::string& leaf, bool overlay_root, const Roots& r,
              std::vector<WIN32_FIND_DATAW>& out, std::set<std::string>& seen) {
    if (root.empty()) return;
    const Resolved res = resolve(root, dirs);
    if (!res.exists) return;
    DIR* d = ::opendir(res.path.c_str());
    if (!d) return;
    while (dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        const bool dot = name == "." || name == "..";
        if (!wildcard_match(leaf, name)) continue;
        if (dot) {
            if (dirs.empty()) continue;
        } else {
            if (overlay_root && dirs.empty() && internal_dir(name)) continue;
            if (!overlay_root) {
                Parts child = dirs;
                child.push_back(name);
                if (deleted(r.overlay, child)) continue;
            }
        }
        if (!seen.insert(lower_ascii(name)).second) continue;
        struct stat st;
        if (::stat((res.path + "/" + name).c_str(), &st) != 0) continue;
        WIN32_FIND_DATAW fd;
        fill_find(name, st, &fd);
        out.push_back(fd);
    }
    ::closedir(d);
}

bool collect(const std::string& pattern, const GuestPath& p, std::vector<WIN32_FIND_DATAW>& out, DWORD& error) {
    Parts dirs;
    std::string leaf;
    const bool trailing = !pattern.empty() && is_separator(pattern.back());
    if (p.parts.empty() || trailing) {
        dirs = p.parts;
        leaf = "*";
    } else {
        dirs.assign(p.parts.begin(), p.parts.end() - 1);
        leaf = p.parts.back();
    }
    const Roots r = roots();
    if (r.game.empty()) {
        error = 3;
        return false;
    }
    const Kind listed = view(r, dirs).kind;
    if (listed != Kind::Directory) {
        error = listed == Kind::File ? kErrDirectory : 3;
        return false;
    }
    std::set<std::string> seen;
    list_dir(r.overlay, dirs, leaf, true, r, out, seen);
    list_dir(r.game, dirs, leaf, false, r, out, seen);
    std::sort(out.begin(), out.end(), [](const WIN32_FIND_DATAW& a, const WIN32_FIND_DATAW& b) {
        return u16icmp(a.cFileName, b.cFileName) < 0;
    });
    if (out.empty()) {
        error = 2;
        return false;
    }
    return true;
}

void to_ansi(const WIN32_FIND_DATAW& wide, WIN32_FIND_DATAA* ansi) {
    std::memset(ansi, 0, sizeof *ansi);
    ansi->dwFileAttributes = wide.dwFileAttributes;
    ansi->ftCreationTime = wide.ftCreationTime;
    ansi->ftLastAccessTime = wide.ftLastAccessTime;
    ansi->ftLastWriteTime = wide.ftLastWriteTime;
    ansi->nFileSizeHigh = wide.nFileSizeHigh;
    ansi->nFileSizeLow = wide.nFileSizeLow;
    const std::string utf8 = narrow(wide.cFileName);
    if (utf8.size() < sizeof ansi->cFileName) std::memcpy(ansi->cFileName, utf8.c_str(), utf8.size() + 1);
}

DWORD WINAPI get_file_attributes_w(const char16_t* name) {
    if (!name) {
        gta6rt_last_error_set(87);
        return INVALID_FILE_ATTRIBUTES;
    }
    const std::string guest = narrow(name);
    const GuestPath p = parse(guest);
    if (p.kind == Parsed::Console && p.parts.empty()) return FILE_ATTRIBUTE_DIRECTORY;
    const Target t = read_target(guest);
    if (t.error) {
        gta6rt_last_error_set(t.error);
        return INVALID_FILE_ATTRIBUTES;
    }
    struct stat st;
    if (::stat(t.path.c_str(), &st) != 0) {
        const int e = errno;
        std::string parent = std::filesystem::path(t.path).parent_path().string();
        struct stat pst;
        gta6rt_last_error_set(e == ENOENT && !parent.empty() && ::stat(parent.c_str(), &pst) == 0 ? 2 : win_error(e));
        return INVALID_FILE_ATTRIBUTES;
    }
    return attributes_of(st);
}

DWORD WINAPI get_file_attributes_a(const char* name) {
    if (!name) {
        gta6rt_last_error_set(87);
        return INVALID_FILE_ATTRIBUTES;
    }
    return get_file_attributes_w(widen(name).c_str());
}

BOOL WINAPI get_file_attributes_ex_w(const char16_t* name, int level, WIN32_FILE_ATTRIBUTE_DATA* info) {
    if (!name || !info || level != 0) {
        gta6rt_last_error_set(87);
        return FALSE;
    }
    const std::string guest = narrow(name);
    const GuestPath p = parse(guest);
    struct stat st;
    if (p.kind == Parsed::Console && p.parts.empty()) {
        const Roots r = roots();
        if (::stat(r.game.c_str(), &st) != 0) {
            gta6rt_last_error_set(3);
            return FALSE;
        }
    } else {
        const Target t = read_target(guest);
        if (t.error) {
            gta6rt_last_error_set(t.error);
            return FALSE;
        }
        if (::stat(t.path.c_str(), &st) != 0) {
            gta6rt_last_error_set(win_error(errno));
            return FALSE;
        }
    }
    std::memset(info, 0, sizeof *info);
    info->dwFileAttributes = attributes_of(st);
    fill_times(st, &info->ftCreationTime, &info->ftLastAccessTime, &info->ftLastWriteTime);
    const uint64_t size = S_ISDIR(st.st_mode) ? 0 : static_cast<uint64_t>(st.st_size);
    info->nFileSizeHigh = static_cast<DWORD>(size >> 32);
    info->nFileSizeLow = static_cast<DWORD>(size);
    return TRUE;
}

BOOL WINAPI get_file_attributes_ex_a(const char* name, int level, WIN32_FILE_ATTRIBUTE_DATA* info) {
    if (!name) {
        gta6rt_last_error_set(87);
        return FALSE;
    }
    return get_file_attributes_ex_w(widen(name).c_str(), level, info);
}

HANDLE find_first(const char16_t* pattern, WIN32_FIND_DATAW* data, int search_op) {
    if (!pattern || !data) {
        gta6rt_last_error_set(87);
        return INVALID_HANDLE_VALUE;
    }
    const std::string guest = narrow(pattern);
    const GuestPath p = parse(guest);
    std::vector<WIN32_FIND_DATAW> entries;
    DWORD error = 0;
    if (p.kind == Parsed::Invalid) {
        gta6rt_last_error_set(3);
        return INVALID_HANDLE_VALUE;
    }
    if (p.kind == Parsed::Refused) {
        gta6rt_last_error_set(123);
        return INVALID_HANDLE_VALUE;
    }
    if (p.kind == Parsed::Host) {
        std::string dir = p.host, leaf = "*";
        const size_t slash = dir.find_last_of('/');
        if (slash != std::string::npos) {
            leaf = dir.substr(slash + 1);
            dir = slash == 0 ? "/" : dir.substr(0, slash);
        } else {
            dir = ".";
        }
        if (leaf.empty()) leaf = "*";
        DIR* d = ::opendir(dir.c_str());
        if (!d) {
            gta6rt_last_error_set(win_error(errno));
            return INVALID_HANDLE_VALUE;
        }
        while (dirent* e = ::readdir(d)) {
            if (!wildcard_match(leaf, e->d_name)) continue;
            struct stat st;
            if (::stat((dir + "/" + e->d_name).c_str(), &st) != 0) continue;
            WIN32_FIND_DATAW fd;
            fill_find(e->d_name, st, &fd);
            entries.push_back(fd);
        }
        ::closedir(d);
        std::sort(entries.begin(), entries.end(), [](const WIN32_FIND_DATAW& a, const WIN32_FIND_DATAW& b) {
            return u16icmp(a.cFileName, b.cFileName) < 0;
        });
        if (entries.empty()) error = 2;
    } else if (!collect(guest, p, entries, error)) {
        entries.clear();
    }
    if (error) {
        gta6rt_last_error_set(error);
        return INVALID_HANDLE_VALUE;
    }
    if (search_op == 2) {
        std::vector<WIN32_FIND_DATAW> dirs_only;
        for (auto& e : entries)
            if (e.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) dirs_only.push_back(e);
        entries = std::move(dirs_only);
        if (entries.empty()) {
            gta6rt_last_error_set(2);
            return INVALID_HANDLE_VALUE;
        }
    }
    auto e = std::make_unique<Enumeration>();
    e->entries = std::move(entries);
    e->next = 1;
    *data = e->entries[0];
    return publish_enumeration(std::move(e));
}

HANDLE WINAPI find_first_file_w(const char16_t* pattern, WIN32_FIND_DATAW* data) { return find_first(pattern, data, 0); }

HANDLE WINAPI find_first_file_ex_w(const char16_t* pattern, int info_level, void* data, int search_op, void* filter, DWORD flags) {
    (void)filter; (void)flags;
    if (info_level != 0 && info_level != 1) {
        gta6rt_last_error_set(87);
        return INVALID_HANDLE_VALUE;
    }
    if (search_op != 0 && search_op != 2) {
        gta6rt_last_error_set(87);
        return INVALID_HANDLE_VALUE;
    }
    return find_first(pattern, static_cast<WIN32_FIND_DATAW*>(data), search_op);
}

HANDLE WINAPI find_first_file_ex_a(const char* pattern, int info_level, void* data, int search_op, void* filter, DWORD flags) {
    if (!pattern || !data) {
        gta6rt_last_error_set(87);
        return INVALID_HANDLE_VALUE;
    }
    WIN32_FIND_DATAW wide{};
    const HANDLE h = find_first_file_ex_w(widen(pattern).c_str(), info_level, &wide, search_op, filter, flags);
    if (h != INVALID_HANDLE_VALUE) to_ansi(wide, static_cast<WIN32_FIND_DATAA*>(data));
    return h;
}

HANDLE WINAPI find_first_file_a(const char* pattern, WIN32_FIND_DATAA* data) {
    if (!pattern || !data) {
        gta6rt_last_error_set(87);
        return INVALID_HANDLE_VALUE;
    }
    WIN32_FIND_DATAW wide{};
    const HANDLE h = find_first_file_w(widen(pattern).c_str(), &wide);
    if (h != INVALID_HANDLE_VALUE) to_ansi(wide, data);
    return h;
}

BOOL WINAPI find_next_file_w(HANDLE find, WIN32_FIND_DATAW* data) {
    if (!find || !data) {
        gta6rt_last_error_set(87);
        return FALSE;
    }
    auto* e = static_cast<Enumeration*>(gta6rt_handle_lookup(reinterpret_cast<uintptr_t>(find), GTA6RT_HANDLE_FIND));
    if (!e) {
        gta6rt_last_error_set(6);
        return FALSE;
    }
    if (e->next >= e->entries.size()) {
        gta6rt_last_error_set(kErrNoMoreFiles);
        return FALSE;
    }
    *data = e->entries[e->next++];
    gta6rt_last_error_set(0);
    return TRUE;
}

BOOL WINAPI find_next_file_a(HANDLE find, WIN32_FIND_DATAA* data) {
    if (!data) {
        gta6rt_last_error_set(87);
        return FALSE;
    }
    WIN32_FIND_DATAW wide{};
    const BOOL ok = find_next_file_w(find, &wide);
    if (ok) to_ansi(wide, data);
    return ok;
}

BOOL WINAPI find_close(HANDLE find) {
    if (!find || !gta6rt_handle_lookup(reinterpret_cast<uintptr_t>(find), GTA6RT_HANDLE_FIND)) {
        gta6rt_last_error_set(6);
        return FALSE;
    }
    gta6rt_handle_close(reinterpret_cast<uintptr_t>(find));
    return TRUE;
}

BOOL fail_bool(DWORD error) {
    gta6rt_last_error_set(error);
    return FALSE;
}

BOOL WINAPI delete_file_w(const char16_t* name) {
    if (!name) return fail_bool(87);
    const GuestPath p = parse(narrow(name));
    if (p.kind == Parsed::Invalid) return fail_bool(3);
    if (p.kind == Parsed::Host) {
        struct stat st;
        if (::lstat(p.host.c_str(), &st) != 0) return fail_bool(win_error(errno));
        if (S_ISDIR(st.st_mode)) return fail_bool(5);
        if (::unlink(p.host.c_str()) != 0) return fail_bool(win_error(errno));
        return TRUE;
    }
    if (p.kind == Parsed::Refused) return fail_bool(123);
    const Roots r = roots();
    if (r.overlay.empty() || p.parts.empty()) return fail_bool(123);

    bool removed = false;
    const Resolved ov = resolve(r.overlay, p.parts);
    if (ov.exists) {
        struct stat st;
        if (::stat(ov.path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return fail_bool(5);
        if (::unlink(ov.path.c_str()) != 0) return fail_bool(win_error(errno));
        removed = true;
    }
    if (!r.game.empty() && !deleted(r.overlay, p.parts)) {
        const Resolved pkg = resolve(r.game, p.parts);
        if (pkg.exists) {
            struct stat st;
            if (::stat(pkg.path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return fail_bool(5);
            if (!removed && ::stat(pkg.path.c_str(), &st) == 0 && !(st.st_mode & S_IWUSR)) return fail_bool(5);
            DWORD error = 0;
            if (!write_marker(marker(r.overlay, p.parts), error)) return fail_bool(error);
            removed = true;
        }
    }
    if (!removed) return fail_bool(missing_error(r, p.parts));
    return TRUE;
}

BOOL WINAPI delete_file_a(const char* name) {
    if (!name) return fail_bool(87);
    return delete_file_w(widen(name).c_str());
}

BOOL create_directory(const char16_t* name) {
    if (!name) return fail_bool(87);
    const GuestPath p = parse(narrow(name));
    if (p.kind == Parsed::Invalid) return fail_bool(3);
    if (p.kind == Parsed::Host) {
        if (::mkdir(p.host.c_str(), 0755) != 0) return fail_bool(errno == EEXIST ? kErrAlreadyExists : win_error(errno));
        return TRUE;
    }
    if (p.kind == Parsed::Refused) return fail_bool(123);
    const Roots r = roots();
    if (r.game.empty() || r.overlay.empty()) return fail_bool(3);
    if (view(r, p.parts).kind != Kind::None) return fail_bool(kErrAlreadyExists);
    if (!is_folder(r, parent_of(p.parts))) return fail_bool(3);
    const Resolved res = resolve(r.overlay, p.parts);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(res.path).parent_path(), ec);
    if (ec) return fail_bool(win_error(ec.value()));
    if (::mkdir(res.path.c_str(), 0755) != 0) return fail_bool(win_error(errno));
    forget_dir_cache(std::filesystem::path(res.path).parent_path().string());
    std::filesystem::remove(marker(r.overlay, p.parts) + "/" + kRemovedDirMarker, ec);
    return TRUE;
}

BOOL WINAPI create_directory_w(const char16_t* name, void*) { return create_directory(name); }
BOOL WINAPI create_directory_a(const char* name, void*) {
    if (!name) return fail_bool(87);
    return create_directory(widen(name).c_str());
}
BOOL WINAPI create_directory_ex_w(const char16_t* tmpl, const char16_t* name, void*) {
    if (!tmpl) return fail_bool(87);
    return create_directory(name);
}

BOOL WINAPI create_directory_ex_a(const char* tmpl, const char* name, void*) {
    if (!tmpl || !name) return fail_bool(87);
    return create_directory(widen(name).c_str());
}

BOOL WINAPI remove_directory_w(const char16_t* name) {
    if (!name) return fail_bool(87);
    const GuestPath p = parse(narrow(name));
    if (p.kind == Parsed::Invalid) return fail_bool(3);
    if (p.kind == Parsed::Host) {
        if (::rmdir(p.host.c_str()) != 0) return fail_bool(win_error(errno));
        return TRUE;
    }
    if (p.kind == Parsed::Refused) return fail_bool(123);
    const Roots r = roots();
    if (r.game.empty() || r.overlay.empty()) return fail_bool(3);
    if (drive_root(p)) return fail_bool(5);
    const View v = view(r, p.parts);
    if (v.kind == Kind::None) return fail_bool(missing_error(r, p.parts));
    if (v.kind == Kind::File) return fail_bool(kErrDirectory);
    if (!empty_in_view(r, p.parts)) return fail_bool(kErrDirNotEmpty);
    if (v.in_overlay && ::rmdir(v.overlay_path.c_str()) != 0) return fail_bool(win_error(errno));
    if (v.in_package) {
        DWORD error = 0;
        if (!write_marker(marker(r.overlay, p.parts) + "/" + kRemovedDirMarker, error)) return fail_bool(error);
    }
    return TRUE;
}

BOOL WINAPI remove_directory_a(const char* name) {
    if (!name) return fail_bool(87);
    return remove_directory_w(widen(name).c_str());
}

bool same_entry(const Parts& a, const Parts& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!same_name(a[i], b[i])) return false;
    return true;
}

constexpr DWORD kReplaceExisting = 1, kCopyAllowed = 2, kDelayUntilReboot = 4;

BOOL WINAPI move_file_ex_w(const char16_t* from, const char16_t* to, DWORD flags) {
    if (!from) return fail_bool(87);
    const GuestPath src = parse(narrow(from));
    const GuestPath dst = to ? parse(narrow(to)) : GuestPath{};
    if (src.kind == Parsed::Invalid || dst.kind == Parsed::Invalid) return fail_bool(3);
    if (src.kind == Parsed::Host && dst.kind == Parsed::Host && to) {
        struct stat st;
        if (::lstat(src.host.c_str(), &st) != 0) return fail_bool(win_error(errno));
        if (::lstat(dst.host.c_str(), &st) == 0 && !(flags & kReplaceExisting)) return fail_bool(kErrAlreadyExists);
        if (::rename(src.host.c_str(), dst.host.c_str()) != 0) return fail_bool(win_error(errno));
        return TRUE;
    }
    if (src.kind == Parsed::Refused || dst.kind == Parsed::Refused) return fail_bool(123);
    if (!to || (flags & kDelayUntilReboot)) return fail_bool(87);
    if (src.kind != dst.kind) return fail_bool(kErrNotSameDevice);

    const Roots r = roots();
    if (r.game.empty() || r.overlay.empty()) return fail_bool(3);
    if (drive_root(src) || drive_root(dst)) return fail_bool(5);
    const View from_view = view(r, src.parts);
    if (from_view.kind == Kind::None) return fail_bool(missing_error(r, src.parts));
    const Resolved overlay_from = resolve(r.overlay, src.parts);
    const Resolved overlay_to = resolve(r.overlay, dst.parts);
    DWORD error = 0;
    if (same_entry(src.parts, dst.parts)) {
        if (src.parts == dst.parts) return TRUE;
        if (from_view.kind == Kind::Directory && from_view.in_package) return fail_bool(5);
        if (from_view.in_overlay) {
            if (::rename(overlay_from.path.c_str(), overlay_to.path.c_str()) != 0) return fail_bool(win_error(errno));
            return TRUE;
        }
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(overlay_to.path).parent_path(), ec);
        if (ec) return fail_bool(win_error(ec.value()));
        if (!copy_file_contents(from_view.package_path, overlay_to.path, false, error)) return fail_bool(error);
        return TRUE;
    }

    const View to_view = view(r, dst.parts);
    if (to_view.kind == Kind::Directory) return fail_bool(5);
    if (to_view.kind == Kind::File && !(flags & kReplaceExisting)) return fail_bool(kErrAlreadyExists);
    if (!is_folder(r, parent_of(dst.parts))) return fail_bool(3);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(overlay_to.path).parent_path(), ec);
    if (ec) return fail_bool(win_error(ec.value()));
    forget_dir_cache(std::filesystem::path(overlay_to.path).parent_path().string());

    if (from_view.kind == Kind::Directory) {
        if (from_view.in_package || to_view.kind != Kind::None) return fail_bool(5);
        if (::rename(overlay_from.path.c_str(), overlay_to.path.c_str()) != 0) return fail_bool(win_error(errno));
        return TRUE;
    }
    if (from_view.in_overlay) {
        if (::rename(overlay_from.path.c_str(), overlay_to.path.c_str()) != 0) return fail_bool(win_error(errno));
    } else if (!copy_file_contents(from_view.package_path, overlay_to.path, (flags & kReplaceExisting) != 0, error)) {
        return fail_bool(error);
    }
    if (from_view.in_package && !write_marker(marker(r.overlay, src.parts), error)) return fail_bool(error);
    return TRUE;
}

BOOL WINAPI move_file_w(const char16_t* from, const char16_t* to) {
    if (!from || !to) return fail_bool(87);
    return move_file_ex_w(from, to, kCopyAllowed);
}
BOOL WINAPI move_file_ex_a(const char* from, const char* to, DWORD flags) {
    if (!from) return fail_bool(87);
    const std::u16string wf = widen(from), wt = to ? widen(to) : std::u16string();
    return move_file_ex_w(wf.c_str(), to ? wt.c_str() : nullptr, flags);
}
BOOL WINAPI move_file_a(const char* from, const char* to) {
    if (!from || !to) return fail_bool(87);
    return move_file_ex_a(from, to, kCopyAllowed);
}

bool copy_destination(const std::string& to, bool fail_if_exists, std::string& out, DWORD& error) {
    const GuestPath p = parse(to);
    if (p.kind == Parsed::Invalid) {
        error = 3;
        return false;
    }
    if (p.kind == Parsed::Host) {
        out = p.host;
        error = 0;
        return true;
    }
    error = 123;
    if (p.kind == Parsed::Refused || p.parts.empty()) return false;
    const Roots r = roots();
    error = 3;
    if (r.game.empty() || r.overlay.empty()) return false;
    const View v = view(r, p.parts);
    error = 5;
    if (v.kind == Kind::Directory) return false;
    error = kErrFileExists;
    if (v.kind == Kind::File && fail_if_exists) return false;
    error = 3;
    if (!is_folder(r, parent_of(p.parts))) return false;
    out = resolve(r.overlay, p.parts).path;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(out).parent_path(), ec);
    if (ec) {
        error = win_error(ec.value());
        return false;
    }
    error = 0;
    return true;
}

BOOL copy_file(const char16_t* from, const char16_t* to, bool fail_if_exists) {
    if (!from || !to) return fail_bool(87);
    const Target t = read_target(narrow(from));
    if (t.error) return fail_bool(t.error);
    std::string dest;
    DWORD error = 0;
    if (!copy_destination(narrow(to), fail_if_exists, dest, error)) return fail_bool(error);
    if (!copy_file_contents(t.path, dest, !fail_if_exists, error)) return fail_bool(error);
    return TRUE;
}

BOOL WINAPI copy_file_w(const char16_t* from, const char16_t* to, BOOL fail_if_exists) { return copy_file(from, to, fail_if_exists != FALSE); }
BOOL WINAPI copy_file_a(const char* from, const char* to, BOOL fail_if_exists) {
    if (!from || !to) return fail_bool(87);
    return copy_file(widen(from).c_str(), widen(to).c_str(), fail_if_exists != FALSE);
}
BOOL WINAPI copy_file_ex_w(const char16_t* from, const char16_t* to, void* progress, void* data, BOOL* cancel, DWORD flags) {
    (void)progress; (void)data;
    if (cancel && *cancel) return fail_bool(1235);
    return copy_file(from, to, (flags & 1) != 0);
}
BOOL WINAPI copy_file_ex_a(const char* from, const char* to, void* progress, void* data, BOOL* cancel, DWORD flags) {
    if (!from || !to) return fail_bool(87);
    return copy_file_ex_w(widen(from).c_str(), widen(to).c_str(), progress, data, cancel, flags);
}

HRESULT WINAPI copy_file2(const char16_t* from, const char16_t* to, COPYFILE2_EXTENDED_PARAMETERS* params) {
    constexpr DWORD kSupportedFlags = 0x1 /* FAIL_IF_EXISTS */ | 0x2 /* RESTARTABLE */ | 0x8 /* ALLOW_DECRYPTED_DESTINATION */ |
                                      0x1000 /* NO_BUFFERING */ | 0x2000 | 0x4000 | 0x40000 | 0x400000 | 0x2000000 | 0x4000000 |
                                      0x8000000 | 0x10000000 | 0x20000000 | 0x80000000u;
    if (!from || !to) return HRESULT_FROM_WIN32(87);
    if (params && params->dwSize < sizeof *params) return E_INVALIDARG;
    if (params && (params->dwCopyFlags & ~kSupportedFlags) != 0) return HRESULT_FROM_WIN32(50);
    if (params && params->pfCancel && *params->pfCancel) return HRESULT_FROM_WIN32(1235);
    if (copy_file(from, to, params && (params->dwCopyFlags & 1) != 0)) return S_OK;
    const DWORD error = gta6rt_last_error_get();
    return HRESULT_FROM_WIN32(error ? error : 31);
}

BOOL WINAPI set_file_attributes_w(const char16_t* name, DWORD attributes) {
    if (!name) return fail_bool(87);
    const GuestPath p = parse(narrow(name));
    if (p.kind == Parsed::Invalid) return fail_bool(3);
    auto apply = [&](const std::string& path) {
        struct stat st;
        if (::stat(path.c_str(), &st) != 0) return fail_bool(win_error(errno));
        mode_t mode = st.st_mode;
        if (attributes & FILE_ATTRIBUTE_READONLY) mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
        else mode |= S_IWUSR;
        if (mode != st.st_mode && ::chmod(path.c_str(), mode & 07777) != 0) return fail_bool(win_error(errno));
        return TRUE;
    };
    if (p.kind == Parsed::Host) return apply(p.host);
    if (p.kind == Parsed::Refused) return fail_bool(123);
    const Roots r = roots();
    if (r.game.empty() || r.overlay.empty()) return fail_bool(3);
    if (p.parts.empty()) return fail_bool(5);
    const View v = view(r, p.parts);
    if (v.kind == Kind::None) return fail_bool(missing_error(r, p.parts));
    std::string target = resolve(r.overlay, p.parts).path;
    if (!v.in_overlay) {
        std::error_code ec;
        if (v.kind == Kind::Directory) {
            std::filesystem::create_directories(target, ec);
            if (ec) return fail_bool(win_error(ec.value()));
        } else {
            const Target t = write_target(narrow(name), true);
            if (t.error) return fail_bool(t.error);
            target = t.path;
        }
    }
    return apply(target);
}

BOOL WINAPI set_file_attributes_a(const char* name, DWORD attributes) {
    if (!name) return fail_bool(87);
    return set_file_attributes_w(widen(name).c_str(), attributes);
}

BOOL WINAPI get_file_information_by_handle(HANDLE h, BY_HANDLE_FILE_INFORMATION* info) {
    const FileRef rf = file_ref(h);
    if (rf.foreign) return g_foreign.get_file_information_by_handle ? g_foreign.get_file_information_by_handle(h, info) : foreign_unavailable();
    FileObj* f = rf.core;
    struct stat st;
    if (!f || !info || f->fd < 0 || ::fstat(f->fd, &st) != 0) {
        gta6rt_last_error_set(f ? 87 : 6);
        return FALSE;
    }
    std::memset(info, 0, sizeof *info);
    info->dwFileAttributes = attributes_of(st);
    fill_times(st, &info->ftCreationTime, &info->ftLastAccessTime, &info->ftLastWriteTime);
    info->dwVolumeSerialNumber = static_cast<DWORD>(st.st_dev);
    info->nFileSizeHigh = static_cast<DWORD>(static_cast<uint64_t>(st.st_size) >> 32);
    info->nFileSizeLow = static_cast<DWORD>(st.st_size);
    info->nNumberOfLinks = st.st_nlink;
    info->nFileIndexHigh = static_cast<DWORD>(st.st_ino >> 32);
    info->nFileIndexLow = static_cast<DWORD>(st.st_ino);
    return TRUE;
}

}  // namespace

void set_foreign_file_ops(const ForeignFileOps& ops) { g_foreign = ops; }

void configure(const std::string& game_dir_utf8, const std::string& overlay_dir_utf8) {
    std::lock_guard lock(g_mutex);
    g_game_dir = game_dir_utf8;
    g_overlay_dir = overlay_dir_utf8;
    while (g_game_dir.size() > 1 && g_game_dir.back() == '/') g_game_dir.pop_back();
    while (g_overlay_dir.size() > 1 && g_overlay_dir.back() == '/') g_overlay_dir.pop_back();
}

std::string game_dir() {
    std::lock_guard lock(g_mutex);
    return g_game_dir;
}

std::string overlay_dir() {
    std::lock_guard lock(g_mutex);
    return g_overlay_dir;
}

HostPath crt_read_path(const std::string& guest) {
    const Target t = read_target(guest);
    return {t.path, t.error};
}

HostPath crt_write_path(const std::string& guest, bool keep_contents) {
    const Target t = write_target(guest, keep_contents);
    return {t.path, t.error};
}

bool crt_remove(const std::string& guest, uint32_t& error) {
    const std::u16string wide = widen(guest.c_str());
    if (delete_file_w(wide.c_str())) return true;
    error = gta6rt_last_error_get();
    return false;
}

}  // namespace rt::fs

namespace {
using namespace rt::fs;
#define F(n, f) SHIM("kernel32.dll", n, f)
}  // namespace

SHIM_TABLE(file_system,
    SHIM("gta6rt.internal", "FileNativeFd", file_native_fd),
    F("CreateFileA", create_file_a), F("CreateFileW", create_file_w), F("CreateFile2", create_file2),
    F("ReadFile", read_file), F("ReadFileEx", read_file_ex), F("ReadFileScatter", read_file_scatter),
    F("WriteFile", write_file), F("GetFileSizeEx", get_file_size_ex), F("GetFileSize", get_file_size),
    F("SetFilePointerEx", set_file_pointer_ex), F("SetFilePointer", set_file_pointer),
    F("SetEndOfFile", set_end_of_file), F("FlushFileBuffers", flush_file_buffers), F("GetFileTime", get_file_time),
    F("SetFileTime", set_file_time), F("GetFileInformationByHandle", get_file_information_by_handle),
    F("FindFirstFileA", find_first_file_a), F("FindFirstFileW", find_first_file_w),
    F("FindFirstFileExA", find_first_file_ex_a), F("FindFirstFileExW", find_first_file_ex_w),
    F("FindNextFileA", find_next_file_a), F("FindNextFileW", find_next_file_w), F("FindClose", find_close),
    F("GetFileAttributesA", get_file_attributes_a), F("GetFileAttributesW", get_file_attributes_w),
    F("GetFileAttributesExA", get_file_attributes_ex_a), F("GetFileAttributesExW", get_file_attributes_ex_w),
    F("DeleteFileA", delete_file_a), F("DeleteFileW", delete_file_w), F("CreateDirectoryA", create_directory_a),
    F("CreateDirectoryW", create_directory_w), F("CreateDirectoryExW", create_directory_ex_w),
    F("CreateDirectoryExA", create_directory_ex_a),
    F("RemoveDirectoryA", remove_directory_a), F("RemoveDirectoryW", remove_directory_w),
    F("MoveFileA", move_file_a), F("MoveFileW", move_file_w), F("MoveFileExA", move_file_ex_a),
    F("MoveFileExW", move_file_ex_w), F("CopyFileA", copy_file_a), F("CopyFileW", copy_file_w),
    F("CopyFileExW", copy_file_ex_w), F("CopyFileExA", copy_file_ex_a), F("CopyFile2", copy_file2), F("SetFileAttributesA", set_file_attributes_a),
    F("SetFileAttributesW", set_file_attributes_w),
);
