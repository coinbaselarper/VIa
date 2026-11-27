#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <gta6rt/os.h>

#include "file_system.h"
#include "shim_call.h"
#include "test.h"

using tcall::call;
namespace fsys = std::filesystem;

namespace {

constexpr const char* kK = "kernel32.dll";
void* const kInvalid = reinterpret_cast<void*>(static_cast<intptr_t>(-1));
constexpr uint32_t kRead = 0x80000000u, kWrite = 0x40000000u;
constexpr uint32_t kCreateNew = 1, kCreateAlways = 2, kOpenExisting = 3, kOpenAlways = 4, kTruncate = 5;

struct Sandbox {
    fsys::path root, game, overlay;
    Sandbox() {
        char tmpl[] = "/tmp/gta6rt-fs-XXXXXX";
        const fsys::path base = fsys::temp_directory_path() / "gta6rt-fs-test";
        fsys::create_directories(base);
        std::string t = (base / "run-XXXXXX").string();
        std::vector<char> buf(t.begin(), t.end());
        buf.push_back(0);
        root = mkdtemp(buf.data());
        (void)tmpl;
        game = root / "game";
        overlay = root / "overlay";
        fsys::create_directories(game / "data" / "sub");
        fsys::create_directories(overlay);
        write(game / "data" / "a.txt", "hello world");
        write(game / "data" / "sub" / "b.bin", "BBBB");
        write(game / "README.TXT", "readme");
        rt::fs::configure(game.string(), overlay.string());
    }
    ~Sandbox() {
        rt::fs::configure("", "");
        std::error_code ec;
        fsys::remove_all(root, ec);
    }
    static void write(const fsys::path& p, const std::string& s) {
        fsys::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << s;
    }
    static std::string read(const fsys::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }
};

void* open(const char16_t* path, uint32_t access, uint32_t disposition, uint32_t flags = 0) {
    return call<void*>(kK, "CreateFileW", path, access, 7u, static_cast<void*>(nullptr), disposition, flags, static_cast<void*>(nullptr));
}

std::string read_all(void* h) {
    std::string out;
    char buf[64];
    for (;;) {
        uint32_t n = 0;
        if (!call<int>(kK, "ReadFile", h, buf, 64u, &n, static_cast<void*>(nullptr)) || n == 0) break;
        out.append(buf, n);
    }
    return out;
}

uint32_t err() { return call<uint32_t>(kK, "GetLastError"); }

std::set<std::u16string> list(const char16_t* pattern) {
    std::set<std::u16string> names;
    std::vector<char16_t> data(296);
    void* h = call<void*>(kK, "FindFirstFileW", pattern, data.data());
    if (h == kInvalid) return names;
    do {
        names.insert(reinterpret_cast<const char16_t*>(data.data() + 22));
    } while (call<int>(kK, "FindNextFileW", h, data.data()));
    call<int>(kK, "FindClose", h);
    return names;
}

uint32_t attrs(const char16_t* p) { return call<uint32_t>(kK, "GetFileAttributesW", p); }

}  // namespace

TEST(fs_reads_package_case_insensitively) {
    Sandbox sb;
    void* h = open(u"GAME:\\data\\a.txt", kRead, kOpenExisting);
    CHECK(h != kInvalid);
    CHECK(read_all(h) == "hello world");
    call<int>(kK, "FlushFileBuffers", h);
    int64_t size = 0;
    CHECK_EQ(call<int>(kK, "GetFileSizeEx", h, &size), 1);
    CHECK_EQ(size, 11);
    gta6rt_handle_close(reinterpret_cast<uintptr_t>(h));
    void* h2 = open(u"game:/DATA/A.TXT", kRead, kOpenExisting);
    CHECK(h2 != kInvalid);
    CHECK(read_all(h2) == "hello world");
    gta6rt_handle_close(reinterpret_cast<uintptr_t>(h2));
    void* h3 = open(u"G:\\readme.txt", kRead, kOpenExisting);
    CHECK(h3 != kInvalid);
    gta6rt_handle_close(reinterpret_cast<uintptr_t>(h3));
    CHECK(open(u"GAME:\\data\\missing.txt", kRead, kOpenExisting) == kInvalid);
    CHECK_EQ(err(), 2u);
    CHECK(open(u"GAME:\\nodir\\missing.txt", kRead, kOpenExisting) == kInvalid);
    CHECK_EQ(err(), 3u);
}

TEST(fs_write_copies_up_and_never_touches_the_package) {
    Sandbox sb;
    void* h = open(u"GAME:\\data\\a.txt", kRead | kWrite, kOpenExisting);
    CHECK(h != kInvalid);
    CHECK(read_all(h) == "hello world");
    int64_t pos = 0;
    CHECK_EQ(call<int>(kK, "SetFilePointerEx", h, int64_t{6}, &pos, 0u), 1);
    CHECK_EQ(pos, 6);
    uint32_t written = 0;
    CHECK_EQ(call<int>(kK, "WriteFile", h, "WORLD!", 6u, &written, static_cast<void*>(nullptr)), 1);
    CHECK_EQ(written, 6u);
    CHECK_EQ(call<int>(kK, "SetEndOfFile", h), 1);
    gta6rt_handle_close(reinterpret_cast<uintptr_t>(h));
    CHECK(Sandbox::read(sb.game / "data" / "a.txt") == "hello world");
    CHECK(Sandbox::read(sb.overlay / "data" / "a.txt") == "hello WORLD!");
    void* r = open(u"GAME:\\data\\a.txt", kRead, kOpenExisting);
    CHECK(read_all(r) == "hello WORLD!");
    gta6rt_handle_close(reinterpret_cast<uintptr_t>(r));
}

TEST(fs_create_dispositions_and_errors) {
    Sandbox sb;
    void* h = open(u"GAME:\\data\\new.txt", kWrite, kCreateNew);
    CHECK(h != kInvalid);
    uint32_t w = 0;
    call<int>(kK, "WriteFile", h, "abc", 3u, &w, static_cast<void*>(nullptr));
    gta6rt_handle_close(reinterpret_cast<uintptr_t>(h));
    CHECK(Sandbox::read(sb.overlay / "data" / "new.txt") == "abc");
    CHECK(open(u"GAME:\\data\\new.txt", kWrite, kCreateNew) == kInvalid);
    CHECK_EQ(err(), 80u);
    void* h2 = open(u"GAME:\\data\\new.txt", kWrite, kCreateAlways);
    CHECK(h2 != kInvalid);
    CHECK_EQ(err(), 183u);
    gta6rt_handle_close(reinterpret_cast<uintptr_t>(h2));
    CHECK(Sandbox::read(sb.overlay / "data" / "new.txt").empty());
    void* h3 = open(u"GAME:\\data\\a.txt", kWrite, kCreateAlways);
    CHECK(h3 != kInvalid);
    gta6rt_handle_close(reinterpret_cast<uintptr_t>(h3));
    CHECK(Sandbox::read(sb.game / "data" / "a.txt") == "hello world");
    CHECK(Sandbox::read(sb.overlay / "data" / "a.txt").empty());
    void* h4 = open(u"GAME:\\data\\x2.txt", kWrite, kOpenAlways);
    CHECK(h4 != kInvalid);
    gta6rt_handle_close(reinterpret_cast<uintptr_t>(h4));
    CHECK(open(u"GAME:\\data\\zz.txt", kWrite, kTruncate) == kInvalid);
    CHECK_EQ(err(), 2u);
    CHECK(open(u"GAME:\\nodir\\q.txt", kWrite, kCreateNew) == kInvalid);
    CHECK_EQ(err(), 3u);
}

TEST(fs_refuses_paths_that_leave_the_root) {
    Sandbox sb;
    CHECK(open(u"GAME:\\..\\secret.txt", kRead, kOpenExisting) == kInvalid);
    CHECK_EQ(err(), 123u);
    CHECK(open(u"GAME:\\data\\..\\..\\x", kRead, kOpenExisting) == kInvalid);
    CHECK_EQ(err(), 123u);
    CHECK(open(u"GAME:\\.gta6rt-deleted\\x", kRead, kOpenExisting) == kInvalid);
    CHECK_EQ(err(), 123u);
    CHECK(open(u"C:\\Windows\\win.ini", kRead, kOpenExisting) == kInvalid);
    CHECK_EQ(err(), 3u);
    CHECK_EQ(attrs(u"GAME:\\data\\..\\README.TXT") != 0xFFFFFFFFu, 1);
}

TEST(fs_delete_hides_package_files_with_a_marker) {
    Sandbox sb;
    CHECK_EQ(call<int>(kK, "DeleteFileW", u"GAME:\\data\\a.txt"), 1);
    CHECK(fsys::exists(sb.game / "data" / "a.txt"));
    CHECK_EQ(attrs(u"GAME:\\data\\a.txt"), 0xFFFFFFFFu);
    CHECK_EQ(err(), 2u);
    CHECK(open(u"GAME:\\data\\a.txt", kRead, kOpenExisting) == kInvalid);
    auto names = list(u"GAME:\\data\\*");
    CHECK(names.count(u"a.txt") == 0 && names.count(u"sub") == 1 && names.count(u".") == 1 && names.count(u"..") == 1);
    CHECK_EQ(call<int>(kK, "DeleteFileW", u"GAME:\\data\\a.txt"), 0);
    CHECK_EQ(err(), 2u);
    void* h = open(u"GAME:\\data\\a.txt", kWrite, kCreateNew);
    CHECK(h != kInvalid);
    gta6rt_handle_close(reinterpret_cast<uintptr_t>(h));
    CHECK(list(u"GAME:\\data\\*").count(u"a.txt") == 1);
    CHECK_EQ(call<int>(kK, "DeleteFileW", u"GAME:\\data\\sub"), 0);
    CHECK_EQ(err(), 5u);
}

TEST(fs_listing_merges_overlay_and_package) {
    Sandbox sb;
    Sandbox::write(sb.overlay / "data" / "extra.txt", "x");
    Sandbox::write(sb.overlay / "data" / "a.txt", "overlay copy!!");
    fsys::create_directories(sb.overlay / ".gta6rt-copy");
    auto names = list(u"GAME:\\data\\*");
    CHECK(names.count(u"a.txt") == 1 && names.count(u"extra.txt") == 1 && names.count(u"sub") == 1);
    CHECK_EQ(names.size(), 5u);
    auto root = list(u"GAME:\\*");
    CHECK(root.count(u"data") == 1 && root.count(u"README.TXT") == 1);
    CHECK(root.count(u".gta6rt-copy") == 0 && root.count(u".gta6rt-deleted") == 0);
    CHECK(list(u"GAME:\\data\\*.txt") == (std::set<std::u16string>{u"a.txt", u"extra.txt"}));
    CHECK(list(u"GAME:\\data\\?.txt") == (std::set<std::u16string>{u"a.txt"}));
    CHECK(list(u"GAME:\\data\\*.nothing").empty());
    CHECK_EQ(err(), 2u);
    void* h = call<void*>(kK, "FindFirstFileW", u"GAME:\\missing\\*", std::vector<char16_t>(296).data());
    CHECK(h == kInvalid);
    CHECK_EQ(err(), 3u);

    std::vector<char16_t> data(296);
    void* f = call<void*>(kK, "FindFirstFileW", u"GAME:\\data\\a.txt", data.data());
    CHECK(f != kInvalid);
    uint32_t attributes, size_low;
    std::memcpy(&attributes, data.data(), 4);
    std::memcpy(&size_low, reinterpret_cast<char*>(data.data()) + 32, 4);
    CHECK_EQ(size_low, 14u);
    CHECK((attributes & 0x10) == 0);
    CHECK_EQ(call<int>(kK, "FindNextFileW", f, data.data()), 0);
    CHECK_EQ(err(), 18u);
    call<int>(kK, "FindClose", f);
    CHECK_EQ(call<int>(kK, "FindClose", f), 0);
}

TEST(fs_directories_and_moves) {
    Sandbox sb;
    CHECK_EQ(call<int>(kK, "CreateDirectoryW", u"GAME:\\data\\made", static_cast<void*>(nullptr)), 1);
    CHECK(fsys::is_directory(sb.overlay / "data" / "made"));
    CHECK_EQ(call<int>(kK, "CreateDirectoryW", u"GAME:\\data\\made", static_cast<void*>(nullptr)), 0);
    CHECK_EQ(err(), 183u);
    CHECK_EQ(call<int>(kK, "CreateDirectoryW", u"GAME:\\nope\\deep", static_cast<void*>(nullptr)), 0);
    CHECK_EQ(err(), 3u);
    CHECK_EQ(attrs(u"GAME:\\data\\made") & 0x10, 0x10u);
    CHECK_EQ(call<int>(kK, "RemoveDirectoryW", u"GAME:\\data\\made"), 1);
    CHECK_EQ(call<int>(kK, "RemoveDirectoryW", u"GAME:\\data\\sub"), 0);
    CHECK_EQ(err(), 145u);
    CHECK_EQ(call<int>(kK, "RemoveDirectoryW", u"GAME:\\"), 0);
    CHECK_EQ(err(), 5u);
    CHECK_EQ(call<int>(kK, "RemoveDirectoryW", u"GAME:\\data\\a.txt"), 0);
    CHECK_EQ(err(), 267u);

    CHECK_EQ(call<int>(kK, "MoveFileExW", u"GAME:\\data\\a.txt", u"GAME:\\data\\moved.txt", 0u), 1);
    CHECK(fsys::exists(sb.game / "data" / "a.txt"));
    CHECK(Sandbox::read(sb.overlay / "data" / "moved.txt") == "hello world");
    CHECK_EQ(attrs(u"GAME:\\data\\a.txt"), 0xFFFFFFFFu);
    CHECK_EQ(attrs(u"GAME:\\data\\moved.txt") != 0xFFFFFFFFu, 1);
    CHECK_EQ(call<int>(kK, "MoveFileExW", u"GAME:\\data\\moved.txt", u"GAME:\\README.TXT", 0u), 0);
    CHECK_EQ(err(), 183u);
    CHECK_EQ(call<int>(kK, "MoveFileExW", u"GAME:\\data\\moved.txt", u"GAME:\\README.TXT", 1u), 1);
    CHECK(Sandbox::read(sb.overlay / "README.TXT") == "hello world");
    CHECK_EQ(call<int>(kK, "MoveFileExW", u"GAME:\\README.TXT", u"/tmp/elsewhere", 1u), 0);
    CHECK_EQ(err(), 17u);
}

TEST(fs_copy_and_attributes) {
    Sandbox sb;
    CHECK_EQ(call<int>(kK, "CopyFileW", u"GAME:\\data\\a.txt", u"GAME:\\data\\copy.txt", 1), 1);
    CHECK(Sandbox::read(sb.overlay / "data" / "copy.txt") == "hello world");
    CHECK_EQ(call<int>(kK, "CopyFileW", u"GAME:\\data\\a.txt", u"GAME:\\data\\copy.txt", 1), 0);
    CHECK_EQ(err(), 80u);
    CHECK_EQ(call<int>(kK, "CopyFileW", u"GAME:\\data\\a.txt", u"GAME:\\data\\copy.txt", 0), 1);
    CHECK_EQ(call<int>(kK, "SetFileAttributesW", u"GAME:\\README.TXT", 1u), 1);
    CHECK_EQ(attrs(u"GAME:\\README.TXT") & 1, 1u);
    CHECK((fsys::status(sb.game / "README.TXT").permissions() & fsys::perms::owner_write) != fsys::perms::none);
    struct Ex {
        uint32_t attrs;
        uint32_t times[6];
        uint32_t high, low;
    } ex{};
    CHECK_EQ(call<int>(kK, "GetFileAttributesExW", u"GAME:\\data\\a.txt", 0, &ex), 1);
    CHECK_EQ(ex.low, 11u);
    CHECK_EQ(call<int>(kK, "GetFileAttributesExW", u"GAME:\\data\\a.txt", 1, &ex), 0);
    CHECK_EQ(attrs(u"GAME:\\"), 0x10u);
}

TEST(fs_scratch_drive_and_host_paths) {
    Sandbox sb;
    void* h = open(u"T:\\work\\tmp.dat", kWrite, kCreateAlways);
    CHECK(h != kInvalid);
    gta6rt_handle_close(reinterpret_cast<uintptr_t>(h));
    CHECK(fsys::exists(sb.overlay / ".gta6rt-temp" / "work" / "tmp.dat"));
    CHECK(list(u"GAME:\\*").count(u".gta6rt-temp") == 0);

    const std::string host = (sb.root / "hostfile.txt").string();
    Sandbox::write(host, "host data");
    const std::u16string whost(host.begin(), host.end());
    void* hh = open(whost.c_str(), kRead, kOpenExisting);
    CHECK(hh != kInvalid);
    CHECK(read_all(hh) == "host data");
    gta6rt_handle_close(reinterpret_cast<uintptr_t>(hh));
    CHECK_EQ(call<int>(kK, "DeleteFileW", whost.c_str()), 1);
    CHECK(!fsys::exists(host));
}

TEST(fs_overlapped_reads_use_the_offset) {
    Sandbox sb;
    void* h = open(u"GAME:\\data\\a.txt", kRead, kOpenExisting, 0x40000000u);
    CHECK(h != kInvalid);
    struct Ov {
        uintptr_t internal, internal_high;
        uint32_t offset, offset_high;
        void* event;
    } ov{};
    ov.offset = 6;
    char buf[16] = {};
    uint32_t n = 0;
    CHECK_EQ(call<int>(kK, "ReadFile", h, buf, 5u, &n, &ov), 1);
    CHECK_EQ(n, 5u);
    CHECK(std::string(buf, 5) == "world");
    CHECK_EQ(ov.internal_high, 5u);
    ov.offset = 11;
    CHECK_EQ(call<int>(kK, "ReadFile", h, buf, 5u, &n, &ov), 0);
    CHECK_EQ(err(), 38u);
    gta6rt_handle_close(reinterpret_cast<uintptr_t>(h));
}

TEST(fs_file_times_and_info) {
    Sandbox sb;
    void* h = open(u"GAME:\\data\\a.txt", kRead, kOpenExisting);
    CHECK(h != kInvalid);
    uint64_t created = 0, accessed = 0, written = 0;
    CHECK_EQ(call<int>(kK, "GetFileTime", h, &created, &accessed, &written), 1);
    CHECK(written > 132000000000000000ull);
    gta6rt_handle_close(reinterpret_cast<uintptr_t>(h));
    void* w = open(u"GAME:\\data\\a.txt", kRead | kWrite, kOpenExisting);
    const uint64_t stamp = 132000000000000000ull;
    CHECK_EQ(call<int>(kK, "SetFileTime", w, static_cast<const uint64_t*>(nullptr), &stamp, &stamp), 1);
    uint64_t got = 0;
    CHECK_EQ(call<int>(kK, "GetFileTime", w, static_cast<uint64_t*>(nullptr), static_cast<uint64_t*>(nullptr), &got), 1);
    CHECK_EQ(got, stamp);
    gta6rt_handle_close(reinterpret_cast<uintptr_t>(w));
    CHECK(Sandbox::read(sb.game / "data" / "a.txt") == "hello world");
}

TEST(fs_crt_paths_follow_the_same_overlay) {
    Sandbox sb;
    void* f = call<void*>("ucrtbase.dll", "fopen", "GAME:\\data\\a.txt", "rb");
    CHECK(f != nullptr);
    char buf[16] = {};
    CHECK_EQ(call<size_t>("ucrtbase.dll", "fread", buf, size_t{1}, size_t{5}, f), 5u);
    CHECK(std::string(buf, 5) == "hello");
    CHECK_EQ(call<int32_t>("ucrtbase.dll", "ftell", f), 5);
    CHECK_EQ(call<int>("ucrtbase.dll", "fseek", f, int32_t{-2}, 2), 0);
    CHECK_EQ(call<int>("ucrtbase.dll", "fgetc", f), 'l');
    CHECK_EQ(call<int>("ucrtbase.dll", "fclose", f), 0);

    void* w = call<void*>("ucrtbase.dll", "fopen", "GAME:\\data\\a.txt", "r+b");
    CHECK(w != nullptr);
    CHECK_EQ(call<size_t>("ucrtbase.dll", "fwrite", "J", size_t{1}, size_t{1}, w), 1u);
    call<int>("ucrtbase.dll", "fclose", w);
    CHECK(Sandbox::read(sb.game / "data" / "a.txt") == "hello world");
    CHECK(Sandbox::read(sb.overlay / "data" / "a.txt") == "Jello world");
    CHECK_EQ(call<int>("ucrtbase.dll", "remove", "GAME:\\data\\a.txt"), 0);
    CHECK(!fsys::exists(sb.overlay / "data" / "a.txt"));
    CHECK(call<void*>("ucrtbase.dll", "fopen", "GAME:\\data\\a.txt", "rb") == nullptr);

    struct Stat {
        uint8_t raw[56];
    } st{};
    CHECK_EQ(call<int>("ucrtbase.dll", "_wstat64", u"GAME:\\data\\sub\\b.bin", &st), 0);
    uint16_t mode;
    int64_t size;
    std::memcpy(&mode, st.raw + 6, 2);
    std::memcpy(&size, st.raw + 24, 8);
    CHECK_EQ(mode & 0xF000, 0x8000);
    CHECK_EQ(size, 4);
}

namespace {

struct Ov {
    uintptr_t internal, internal_high;
    uint32_t offset, offset_high;
    void* event;
};

struct FakeCalls {
    int read_file = 0, write_file = 0, size_ex = 0, seek_ex = 0, eof = 0, flush = 0, get_time = 0, set_time = 0, info = 0,
        read_ex = 0, scatter = 0;
    void* last_handle = nullptr;
} g_fake;

BOOL WINAPI fake_read(HANDLE h, void*, DWORD, DWORD* n, void*) { ++g_fake.read_file; g_fake.last_handle = h; *n = 7; return TRUE; }
BOOL WINAPI fake_write(HANDLE h, const void*, DWORD, DWORD* n, void*) { ++g_fake.write_file; g_fake.last_handle = h; *n = 9; return TRUE; }
BOOL WINAPI fake_size(HANDLE, int64_t* s) { ++g_fake.size_ex; *s = 1234; return TRUE; }
BOOL WINAPI fake_seek(HANDLE, int64_t d, int64_t* o, DWORD) { ++g_fake.seek_ex; *o = d + 1; return TRUE; }
BOOL WINAPI fake_eof(HANDLE) { ++g_fake.eof; return TRUE; }
BOOL WINAPI fake_flush(HANDLE) { ++g_fake.flush; return TRUE; }
BOOL WINAPI fake_get_time(HANDLE, FILETIME*, FILETIME*, FILETIME*) { ++g_fake.get_time; return TRUE; }
BOOL WINAPI fake_set_time(HANDLE, const FILETIME*, const FILETIME*, const FILETIME*) { ++g_fake.set_time; return TRUE; }
BOOL WINAPI fake_info(HANDLE, BY_HANDLE_FILE_INFORMATION*) { ++g_fake.info; return TRUE; }
BOOL WINAPI fake_read_ex(HANDLE, void*, DWORD, void*, void*) { ++g_fake.read_ex; return TRUE; }
BOOL WINAPI fake_scatter(HANDLE, void*, DWORD, DWORD*, void*) { ++g_fake.scatter; return TRUE; }

struct Completion {
    int calls = 0;
    uint32_t error = 0xFFFF, bytes = 0;
    void* ov = nullptr;
} g_completion;

void GTA6RT_GUEST_ABI completion_routine(uint32_t error, uint32_t bytes, void* ov) {
    ++g_completion.calls;
    g_completion.error = error;
    g_completion.bytes = bytes;
    g_completion.ov = ov;
}

void* g_pending_apc_data = nullptr;
void (WINAPI* g_pending_apc_fn)(ULONG_PTR) = nullptr;
DWORD WINAPI fake_queue_apc(void* fn, HANDLE, ULONG_PTR data) {
    g_pending_apc_fn = reinterpret_cast<void(WINAPI*)(ULONG_PTR)>(fn);
    g_pending_apc_data = reinterpret_cast<void*>(data);
    return 1;
}
HANDLE WINAPI fake_current_thread() { return reinterpret_cast<HANDLE>(static_cast<intptr_t>(-2)); }

}  // namespace

TEST(fs_handles_not_created_by_core_are_never_cast_to_file_objects) {
    struct alignas(16) Foreign {
        unsigned char bytes[128];
    } foreign{};
    uint64_t raw = 0;
    CHECK(gta6rt_handle_alloc(GTA6RT_HANDLE_FILE, &foreign, nullptr, &raw) == GTA6RT_OK);
    void* h = reinterpret_cast<void*>(static_cast<uintptr_t>(raw));
    rt::fs::set_foreign_file_ops({});
    char buf[8] = {};
    uint32_t n = 99;
    int64_t sz = 77, pos = 0;
    uint64_t ft[2] = {};
    uint32_t info[13] = {};
    CHECK_EQ(call<int>(kK, "ReadFile", h, buf, 8u, &n, static_cast<void*>(nullptr)), 0);
    CHECK_EQ(err(), 6u);
    CHECK_EQ(n, 0u);
    CHECK_EQ(call<int>(kK, "WriteFile", h, buf, 8u, &n, static_cast<void*>(nullptr)), 0);
    CHECK_EQ(err(), 6u);
    CHECK_EQ(call<int>(kK, "GetFileSizeEx", h, &sz), 0);
    CHECK_EQ(err(), 6u);
    CHECK_EQ(sz, 77);
    CHECK_EQ(call<uint32_t>(kK, "GetFileSize", h, static_cast<uint32_t*>(nullptr)), 0xFFFFFFFFu);
    CHECK_EQ(call<int>(kK, "SetFilePointerEx", h, int64_t{4}, &pos, 0u), 0);
    CHECK_EQ(err(), 6u);
    CHECK_EQ(call<int>(kK, "SetEndOfFile", h), 0);
    CHECK_EQ(call<int>(kK, "FlushFileBuffers", h), 0);
    CHECK_EQ(call<int>(kK, "GetFileTime", h, &ft[0], &ft[1], static_cast<void*>(nullptr)), 0);
    CHECK_EQ(call<int>(kK, "SetFileTime", h, &ft[0], &ft[1], static_cast<void*>(nullptr)), 0);
    CHECK_EQ(call<int>(kK, "GetFileInformationByHandle", h, info), 0);
    CHECK_EQ(err(), 6u);
    Ov ov{};
    CHECK_EQ(call<int>(kK, "ReadFileEx", h, buf, 8u, &ov, reinterpret_cast<void*>(&completion_routine)), 0);
    CHECK_EQ(err(), 6u);
    CHECK_EQ(call<int>(kK, "ReadFileScatter", h, buf, 4096u, static_cast<uint32_t*>(nullptr), &ov), 0);
    CHECK_EQ(err(), 6u);
    for (unsigned char b : foreign.bytes) CHECK_EQ(b, 0);

    rt::fs::ForeignFileOps ops{};
    ops.read_file = fake_read;
    ops.write_file = fake_write;
    ops.get_file_size_ex = fake_size;
    ops.set_file_pointer_ex = fake_seek;
    ops.set_end_of_file = fake_eof;
    ops.flush_file_buffers = fake_flush;
    ops.get_file_time = fake_get_time;
    ops.set_file_time = fake_set_time;
    ops.get_file_information_by_handle = fake_info;
    ops.read_file_ex = fake_read_ex;
    ops.read_file_scatter = fake_scatter;
    rt::fs::set_foreign_file_ops(ops);
    g_fake = {};
    CHECK_EQ(call<int>(kK, "ReadFile", h, buf, 8u, &n, static_cast<void*>(nullptr)), 1);
    CHECK_EQ(n, 7u);
    CHECK(g_fake.last_handle == h);
    CHECK_EQ(call<int>(kK, "WriteFile", h, buf, 8u, &n, static_cast<void*>(nullptr)), 1);
    CHECK_EQ(n, 9u);
    CHECK_EQ(call<int>(kK, "GetFileSizeEx", h, &sz), 1);
    CHECK_EQ(sz, 1234);
    CHECK_EQ(call<uint32_t>(kK, "GetFileSize", h, static_cast<uint32_t*>(nullptr)), 1234u);
    CHECK_EQ(call<int>(kK, "SetFilePointerEx", h, int64_t{4}, &pos, 0u), 1);
    CHECK_EQ(pos, 5);
    CHECK_EQ(call<int>(kK, "SetEndOfFile", h), 1);
    CHECK_EQ(call<int>(kK, "FlushFileBuffers", h), 1);
    CHECK_EQ(call<int>(kK, "GetFileTime", h, &ft[0], &ft[1], static_cast<void*>(nullptr)), 1);
    CHECK_EQ(call<int>(kK, "SetFileTime", h, &ft[0], &ft[1], static_cast<void*>(nullptr)), 1);
    CHECK_EQ(call<int>(kK, "GetFileInformationByHandle", h, info), 1);
    CHECK_EQ(call<int>(kK, "ReadFileEx", h, buf, 8u, &ov, reinterpret_cast<void*>(&completion_routine)), 1);
    CHECK_EQ(call<int>(kK, "ReadFileScatter", h, buf, 4096u, static_cast<uint32_t*>(nullptr), &ov), 1);
    CHECK_EQ(g_fake.read_file, 1);
    CHECK_EQ(g_fake.write_file, 1);
    CHECK_EQ(g_fake.size_ex, 2);
    CHECK_EQ(g_fake.seek_ex, 1);
    CHECK_EQ(g_fake.eof + g_fake.flush + g_fake.get_time + g_fake.set_time + g_fake.info + g_fake.read_ex + g_fake.scatter, 7);
    for (unsigned char b : foreign.bytes) CHECK_EQ(b, 0);
    rt::fs::set_foreign_file_ops({});
    gta6rt_handle_close(raw);
}

TEST(fs_core_handles_keep_working_beside_foreign_handles) {
    Sandbox sb;
    uint64_t raw = 0;
    char other[64] = {};
    CHECK(gta6rt_handle_alloc(GTA6RT_HANDLE_FILE, other, nullptr, &raw) == GTA6RT_OK);
    void* h = open(u"GAME:\\data\\a.txt", kRead, kOpenExisting);
    CHECK(h != kInvalid);
    CHECK(read_all(h) == "hello world");
    int64_t sz = 0;
    CHECK_EQ(call<int>(kK, "GetFileSizeEx", h, &sz), 1);
    CHECK_EQ(sz, 11);
    gta6rt_handle_close(reinterpret_cast<uintptr_t>(h));
    CHECK_EQ(call<int>(kK, "GetFileSizeEx", h, &sz), 0);
    CHECK_EQ(err(), 6u);
    gta6rt_handle_close(raw);
}

TEST(fs_create_file2_copy_file2_and_the_rest_of_the_family) {
    Sandbox sb;
    struct Params {
        uint32_t size = 32, attrs = 0, flags = 0, qos = 0;
        void* security = nullptr;
        void* tmpl = nullptr;
    } params;
    void* h = call<void*>(kK, "CreateFile2", u"GAME:\\data\\a.txt", kRead, 1u, kOpenExisting, &params);
    CHECK(h != kInvalid);
    CHECK(read_all(h) == "hello world");
    int64_t sz = 0;
    CHECK_EQ(call<int>(kK, "GetFileSizeEx", h, &sz), 1);
    CHECK_EQ(sz, 11);
    gta6rt_handle_close(reinterpret_cast<uintptr_t>(h));
    void* none = call<void*>(kK, "CreateFile2", u"GAME:\\data\\a.txt", kRead, 1u, kOpenExisting, static_cast<void*>(nullptr));
    CHECK(none != kInvalid);
    gta6rt_handle_close(reinterpret_cast<uintptr_t>(none));
    CHECK(call<void*>(kK, "CreateFile2", u"GAME:\\data\\missing.txt", kRead, 1u, kOpenExisting, &params) == kInvalid);
    CHECK_EQ(err(), 2u);
    params.size = 8;
    CHECK(call<void*>(kK, "CreateFile2", u"GAME:\\data\\a.txt", kRead, 1u, kOpenExisting, &params) == kInvalid);
    CHECK_EQ(err(), 87u);
    params.size = 32;
    void* w = call<void*>(kK, "CreateFile2", u"T:\\created.txt", kWrite, 0u, kCreateAlways, &params);
    CHECK(w != kInvalid);
    uint32_t n = 0;
    CHECK_EQ(call<int>(kK, "WriteFile", w, "abc", 3u, &n, static_cast<void*>(nullptr)), 1);
    gta6rt_handle_close(reinterpret_cast<uintptr_t>(w));
    CHECK(Sandbox::read(sb.overlay / ".gta6rt-temp" / "created.txt") == "abc");

    struct Copy2 {
        uint32_t size = 40, flags = 0;
        int* cancel = nullptr;
        void* progress = nullptr;
        void* context = nullptr;
    } c2;
    CHECK_EQ(call<int32_t>(kK, "CopyFile2", u"GAME:\\data\\a.txt", u"T:\\c2.txt", &c2), 0);
    CHECK(Sandbox::read(sb.overlay / ".gta6rt-temp" / "c2.txt") == "hello world");
    c2.flags = 1;
    CHECK_EQ(static_cast<uint32_t>(call<int32_t>(kK, "CopyFile2", u"GAME:\\data\\a.txt", u"T:\\c2.txt", &c2)), 0x80070050u);
    c2.flags = 0x80;
    CHECK_EQ(static_cast<uint32_t>(call<int32_t>(kK, "CopyFile2", u"GAME:\\data\\a.txt", u"T:\\c3.txt", &c2)), 0x80070032u);
    c2.flags = 0;
    int cancel = 1;
    c2.cancel = &cancel;
    CHECK_EQ(static_cast<uint32_t>(call<int32_t>(kK, "CopyFile2", u"GAME:\\data\\a.txt", u"T:\\c4.txt", &c2)), 0x800704D3u);
    c2.size = 8;
    CHECK_EQ(static_cast<uint32_t>(call<int32_t>(kK, "CopyFile2", u"GAME:\\data\\a.txt", u"T:\\c5.txt", &c2)), 0x80070057u);
    CHECK_EQ(call<int32_t>(kK, "CopyFile2", u"GAME:\\data\\a.txt", u"T:\\c6.txt", static_cast<void*>(nullptr)), 0);

    CHECK_EQ(call<int>(kK, "CopyFileExA", "GAME:\\data\\a.txt", "T:\\ex.txt", static_cast<void*>(nullptr), static_cast<void*>(nullptr),
                       static_cast<void*>(nullptr), 0u), 1);
    CHECK(Sandbox::read(sb.overlay / ".gta6rt-temp" / "ex.txt") == "hello world");
    CHECK_EQ(call<int>(kK, "CopyFileExA", "GAME:\\data\\a.txt", "T:\\ex.txt", static_cast<void*>(nullptr), static_cast<void*>(nullptr),
                       static_cast<void*>(nullptr), 1u), 0);
    CHECK_EQ(err(), 80u);
    CHECK_EQ(call<int>(kK, "CreateDirectoryExA", "GAME:\\data", "T:\\madedir", static_cast<void*>(nullptr)), 1);
    CHECK(fsys::is_directory(sb.overlay / ".gta6rt-temp" / "madedir"));
}

TEST(fs_read_file_scatter_and_read_file_ex_on_core_handles) {
    Sandbox sb;
    std::string big(6000, 'x');
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>('a' + i % 26);
    Sandbox::write(sb.game / "data" / "big.bin", big);
    void* h = open(u"GAME:\\data\\big.bin", kRead, kOpenExisting, 0x40000000u);
    CHECK(h != kInvalid);
    alignas(4096) static char pages[2][4096];
    std::memset(pages, 0, sizeof pages);
    uint64_t seg[3] = {reinterpret_cast<uintptr_t>(pages[0]), reinterpret_cast<uintptr_t>(pages[1]), 0};
    Ov ov{};
    CHECK_EQ(call<int>(kK, "ReadFileScatter", h, seg, 8192u, static_cast<uint32_t*>(nullptr), &ov), 1);
    CHECK_EQ(ov.internal_high, 6000u);
    CHECK(std::memcmp(pages[0], big.data(), 4096) == 0);
    CHECK(std::memcmp(pages[1], big.data() + 4096, 1904) == 0);
    CHECK_EQ(call<int>(kK, "ReadFileScatter", h, seg, 100u, static_cast<uint32_t*>(nullptr), &ov), 0);
    CHECK_EQ(err(), 87u);
    CHECK_EQ(call<int>(kK, "ReadFileScatter", h, seg, 4096u, static_cast<uint32_t*>(nullptr), static_cast<void*>(nullptr)), 0);
    CHECK_EQ(err(), 87u);

    char buf[8] = {};
    Ov rv{};
    rv.offset = 4;
    rt::fs::set_foreign_file_ops({});
    CHECK_EQ(call<int>(kK, "ReadFileEx", h, buf, 5u, &rv, reinterpret_cast<void*>(&completion_routine)), 0);
    CHECK_EQ(err(), 50u);

    rt::fs::ForeignFileOps ops{};
    ops.queue_user_apc = fake_queue_apc;
    ops.get_current_thread = fake_current_thread;
    rt::fs::set_foreign_file_ops(ops);
    g_completion = {};
    g_pending_apc_fn = nullptr;
    CHECK_EQ(call<int>(kK, "ReadFileEx", h, buf, 5u, &rv, reinterpret_cast<void*>(&completion_routine)), 1);
    CHECK(std::string(buf, 5) == big.substr(4, 5));
    CHECK_EQ(g_completion.calls, 0);
    CHECK(g_pending_apc_fn != nullptr);
    g_pending_apc_fn(reinterpret_cast<ULONG_PTR>(g_pending_apc_data));
    CHECK_EQ(g_completion.calls, 1);
    CHECK_EQ(g_completion.error, 0u);
    CHECK_EQ(g_completion.bytes, 5u);
    CHECK(g_completion.ov == &rv);
    CHECK_EQ(rv.internal_high, 5u);

    rv.offset = 6000;
    g_pending_apc_fn = nullptr;
    CHECK_EQ(call<int>(kK, "ReadFileEx", h, buf, 5u, &rv, reinterpret_cast<void*>(&completion_routine)), 1);
    g_pending_apc_fn(reinterpret_cast<ULONG_PTR>(g_pending_apc_data));
    CHECK_EQ(g_completion.error, 38u);
    CHECK_EQ(g_completion.bytes, 0u);
    rt::fs::set_foreign_file_ops({});
    gta6rt_handle_close(reinterpret_cast<uintptr_t>(h));
}
