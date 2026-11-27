#include "t.h"

#include <atomic>
#include <cstdlib>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static std::string g_tmp;

static std::u16string W(const std::string& s) {
    std::u16string w;
    for (unsigned char c : s) w.push_back((char16_t)c);
    return w;
}

static std::string host(const char* win) {
    char buf[4096];
    if (gta6rt_path_to_host(win, buf, sizeof buf) != GTA6RT_OK) return "<err>";
    return buf;
}

static std::string to_win(const std::string& h) {
    char buf[4096];
    if (gta6rt_path_to_win(h.c_str(), buf, sizeof buf) != GTA6RT_OK) return "<err>";
    return buf;
}

// The data volume is case-insensitive APFS, where the input spelling already resolves; on a case-sensitive
// volume resolve_case must return the on-disk spelling. Either way both names must reach the same inode.
static bool same_file(const std::string& got, const std::string& want) {
    if (strcasecmp(got.c_str(), want.c_str()) != 0) {
        fprintf(stderr, "   got  %s\n   want %s\n", got.c_str(), want.c_str());
        return false;
    }
    struct stat a, b;
    bool ea = stat(got.c_str(), &a) == 0, eb = stat(want.c_str(), &b) == 0;
    return ea == eb && (!ea || (a.st_ino == b.st_ino && a.st_dev == b.st_dev));
}

static void write_host_file(const std::string& p, const std::string& data) {
    FILE* f = fopen(p.c_str(), "wb");
    if (f) {
        fwrite(data.data(), 1, data.size(), f);
        fclose(f);
    }
}

static void test_paths() {
    SECTION("path translation");
    gta6rt_path_set_root((g_tmp + "/root/").c_str());
    mkdir((g_tmp + "/root").c_str(), 0755);
    mkdir((g_tmp + "/root/c").c_str(), 0755);
    mkdir((g_tmp + "/game").c_str(), 0755);
    mkdir((g_tmp + "/game/Data").c_str(), 0755);
    write_host_file(g_tmp + "/game/Data/File.TXT", "x");
    gta6rt_path_map_prefix("GAME:", (g_tmp + "/game").c_str());
    gta6rt_path_map_prefix("C:\\Users\\Player\\Saves\\", (g_tmp + "/saves").c_str());

    CHECK(host("C:\\foo\\bar.txt") == g_tmp + "/root/c/foo/bar.txt");
    CHECK(host("c:/foo/./x/../bar.txt") == g_tmp + "/root/c/foo/bar.txt");
    CHECK(host("D:\\a") == g_tmp + "/root/d/a");
    CHECK(same_file(host("GAME:\\data\\file.txt"), g_tmp + "/game/Data/File.TXT"));
    CHECK(same_file(host("game:\\DATA\\missing.bin"), g_tmp + "/game/Data/missing.bin"));
    CHECK(same_file(host("GAME:data\\File.txt"), g_tmp + "/game/Data/File.TXT"));
    CHECK(host("c:\\users\\player\\saves\\slot0.sav") == g_tmp + "/saves/slot0.sav");
    CHECK(host("C:\\Users\\Player\\SavesX\\a") == g_tmp + "/root/c/Users/Player/SavesX/a");
    CHECK(host("\\\\?\\C:\\foo\\bar") == g_tmp + "/root/c/foo/bar");
    CHECK(host("Z:\\usr\\bin") == "/usr/bin");
    CHECK(host("/usr/bin/../lib") == "/usr/lib");
    CHECK(host("C:\\dir.\\name. ") == g_tmp + "/root/c/dir/name");
    CHECK(host("rel\\path\\f") == "rel/path/f");
    char buf[8];
    CHECK_EQ(gta6rt_path_to_host("C:\\long\\path", buf, sizeof buf), GTA6RT_E_INVALID_ARG);
    CHECK_EQ(gta6rt_path_to_host("\\\\server\\share\\x", buf, sizeof buf), GTA6RT_E_NOT_FOUND);

    CHECK(to_win(g_tmp + "/game/Data/File.TXT") == "GAME:\\Data\\File.TXT");
    CHECK(to_win(g_tmp + "/root/c/foo") == "C:\\foo");
    CHECK(to_win(g_tmp + "/root/c") == "C:\\");
    CHECK(to_win(g_tmp + "/saves/a.sav") == "C:\\Users\\Player\\Saves\\a.sav");
    CHECK(to_win("/usr/bin") == "Z:\\usr\\bin");

    gta6rt_path_map_prefix("C:\\Users\\Player\\Saves", nullptr);
    CHECK(host("c:\\users\\player\\saves\\slot0.sav") == g_tmp + "/root/c/users/player/saves/slot0.sav");
}

static void test_create_dispositions() {
    SECTION("CreateFileW dispositions and errors");
    HANDLE h = CreateFileW(u"C:\\new.bin", GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), ERROR_SUCCESS);
    CloseHandle(h);
    h = CreateFileW(u"C:\\new.bin", GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    CHECK(h == INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), ERROR_FILE_EXISTS);
    h = CreateFileW(u"C:\\new.bin", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), ERROR_ALREADY_EXISTS);
    CloseHandle(h);
    h = CreateFileW(u"C:\\new.bin", GENERIC_READ, 0, nullptr, OPEN_ALWAYS, 0, nullptr);
    CHECK_EQ(GetLastError(), ERROR_ALREADY_EXISTS);
    CloseHandle(h);
    h = CreateFileW(u"C:\\other.bin", GENERIC_READ, 0, nullptr, OPEN_ALWAYS, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), ERROR_SUCCESS);
    CloseHandle(h);
    h = CreateFileW(u"C:\\missing.bin", GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(h == INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), ERROR_FILE_NOT_FOUND);
    h = CreateFileW(u"C:\\nodir\\missing.bin", GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(h == INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), ERROR_PATH_NOT_FOUND);
    h = CreateFileW(u"C:\\new.bin", GENERIC_READ, 0, nullptr, TRUNCATE_EXISTING, 0, nullptr);
    CHECK(h == INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), ERROR_INVALID_PARAMETER);
    h = CreateFileW(u"Q:\\x", GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(h == INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), ERROR_PATH_NOT_FOUND);
    h = CreateFileW(u"C:\\", GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(h == INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), ERROR_ACCESS_DENIED);
    h = CreateFileW(u"C:\\", GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    CHECK_EQ(GetFileType(h), FILE_TYPE_DISK);
    CloseHandle(h);
    h = CreateFileA("c:\\NEW.BIN", GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    CloseHandle(h);
}

static void test_sync_io() {
    SECTION("synchronous ReadFile/WriteFile, pointer, size, EOF");
    HANDLE h = CreateFileW(u"C:\\sync.bin", GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    char data[1000];
    for (int i = 0; i < 1000; i++) data[i] = (char)(i * 13);
    DWORD n = 0;
    CHECK(WriteFile(h, data, 1000, &n, nullptr));
    CHECK_EQ(n, 1000);
    LARGE_INTEGER sz, pos, d;
    CHECK(GetFileSizeEx(h, &sz));
    CHECK_EQ(sz.QuadPart, 1000);
    DWORD hi = 7;
    CHECK_EQ(GetFileSize(h, &hi), 1000);
    CHECK_EQ(hi, 0);
    d.QuadPart = 0;
    CHECK(SetFilePointerEx(h, d, &pos, FILE_CURRENT));
    CHECK_EQ(pos.QuadPart, 1000);
    CHECK_EQ(SetFilePointer(h, 100, nullptr, FILE_BEGIN), 100);
    char back[2000] = {};
    CHECK(ReadFile(h, back, 50, &n, nullptr));
    CHECK_EQ(n, 50);
    CHECK(memcmp(back, data + 100, 50) == 0);
    d.QuadPart = -10;
    CHECK(SetFilePointerEx(h, d, &pos, FILE_END));
    CHECK_EQ(pos.QuadPart, 990);
    CHECK(ReadFile(h, back, 2000, &n, nullptr));
    CHECK_EQ(n, 10);
    CHECK(ReadFile(h, back, 2000, &n, nullptr));
    CHECK_EQ(n, 0);
    d.QuadPart = -2000;
    CHECK(!SetFilePointerEx(h, d, &pos, FILE_CURRENT));
    CHECK_EQ(GetLastError(), ERROR_NEGATIVE_SEEK);

    OVERLAPPED o = {};
    o.Offset = 500;
    CHECK(ReadFile(h, back, 20, &n, &o));
    CHECK_EQ(n, 20);
    CHECK(memcmp(back, data + 500, 20) == 0);
    CHECK_EQ(o.InternalHigh, 20);
    o.Offset = 5000;
    CHECK(!ReadFile(h, back, 20, &n, &o));
    CHECK_EQ(GetLastError(), ERROR_HANDLE_EOF);

    CHECK_EQ(SetFilePointer(h, 300, nullptr, FILE_BEGIN), 300);
    CHECK(SetEndOfFile(h));
    CHECK(GetFileSizeEx(h, &sz));
    CHECK_EQ(sz.QuadPart, 300);
    CHECK(FlushFileBuffers(h));

    FILE_STANDARD_INFO si;
    CHECK(GetFileInformationByHandleEx(h, FileStandardInfo, &si, sizeof si));
    CHECK_EQ(si.EndOfFile.QuadPart, 300);
    CHECK(!si.Directory);
    BY_HANDLE_FILE_INFORMATION bi;
    CHECK(GetFileInformationByHandle(h, &bi));
    CHECK_EQ(bi.nFileSizeLow, 300);
    CHECK_EQ(bi.nNumberOfLinks, 1);
    FILETIME c, a, w;
    CHECK(GetFileTime(h, &c, &a, &w));
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    uint64_t wt = ((uint64_t)w.dwHighDateTime << 32) | w.dwLowDateTime;
    uint64_t nt = ((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime;
    CHECK(wt <= nt && nt - wt < 600ull * 10000000ull);
    WCHAR fp[1024];
    DWORD fl = GetFinalPathNameByHandleW(h, fp, 1024, 0);
    CHECK(fl > 0 && fl < 1024);
    std::u16string fps(fp, fl);
    CHECK(fps == u"\\\\?\\C:\\sync.bin");
    CloseHandle(h);

    h = CreateFileW(u"C:\\sync.bin", GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(!WriteFile(h, data, 1, &n, nullptr));
    CHECK_EQ(GetLastError(), ERROR_ACCESS_DENIED);
    CloseHandle(h);
    h = CreateFileW(u"C:\\append.bin", FILE_APPEND_DATA, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    CHECK(WriteFile(h, "abc", 3, &n, nullptr));
    CHECK_EQ(SetFilePointer(h, 0, nullptr, FILE_BEGIN), 0);
    CHECK(WriteFile(h, "def", 3, &n, nullptr));
    CloseHandle(h);
    h = CreateFileW(u"C:\\append.bin", GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(ReadFile(h, back, 100, &n, nullptr));
    CHECK_EQ(n, 6);
    CHECK(memcmp(back, "abcdef", 6) == 0);
    CloseHandle(h);
}

static void make_pattern_file(const char16_t* name, size_t size) {
    HANDLE h = CreateFileW(name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    std::vector<uint32_t> v(size / 4);
    for (size_t i = 0; i < v.size(); i++) v[i] = (uint32_t)(i * 2654435761u);
    DWORD n;
    WriteFile(h, v.data(), (DWORD)size, &n, nullptr);
    CloseHandle(h);
}

static bool pattern_ok(const void* buf, size_t off, size_t n) {
    const uint32_t* p = (const uint32_t*)buf;
    for (size_t i = 0; i < n / 4; i++)
        if (p[i] != (uint32_t)((off / 4 + i) * 2654435761u)) return false;
    return true;
}

static std::atomic<int> g_routine_calls;
static std::atomic<DWORD> g_routine_bytes;
static void WINAPI io_routine(DWORD err, DWORD bytes, LPOVERLAPPED) {
    if (err == 0) g_routine_calls++;
    g_routine_bytes = bytes;
}

static void test_overlapped() {
    SECTION("overlapped I/O, completion ports, ReadFileEx");
    const size_t kSize = 4u << 20, kChunk = 64u << 10;
    make_pattern_file(u"C:\\pattern.bin", kSize);
    HANDLE h = CreateFileW(u"C:\\pattern.bin", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    const int kN = (int)(kSize / kChunk);
    std::vector<OVERLAPPED> ov(kN);
    std::vector<HANDLE> ev(kN);
    std::vector<char> buf(kSize);
    int pending = 0;
    for (int i = 0; i < kN; i++) {
        memset(&ov[i], 0, sizeof ov[i]);
        ov[i].Offset = (DWORD)(i * kChunk);
        ev[i] = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ov[i].hEvent = ev[i];
        BOOL r = ReadFile(h, &buf[i * kChunk], (DWORD)kChunk, nullptr, &ov[i]);
        if (!r && GetLastError() == ERROR_IO_PENDING) pending++;
    }
    CHECK_EQ(pending, kN);
    bool all_ok = true;
    for (int i = 0; i < kN; i++) {
        DWORD n = 0;
        if (!GetOverlappedResult(h, &ov[i], &n, TRUE) || n != kChunk) all_ok = false;
        CloseHandle(ev[i]);
    }
    CHECK(all_ok);
    CHECK(pattern_ok(buf.data(), 0, kSize));

    OVERLAPPED eo = {};
    eo.Offset = (DWORD)kSize + 10;
    eo.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD n = 1;
    CHECK(!ReadFile(h, buf.data(), 100, nullptr, &eo));
    CHECK_EQ(GetLastError(), ERROR_IO_PENDING);
    CHECK(!GetOverlappedResult(h, &eo, &n, TRUE));
    CHECK_EQ(GetLastError(), ERROR_HANDLE_EOF);
    CHECK_EQ(n, 0);
    CloseHandle(eo.hEvent);

    OVERLAPPED no = {};
    no.Offset = 4096;
    CHECK(!ReadFile(h, buf.data(), 4096, nullptr, &no));
    CHECK(GetOverlappedResult(h, &no, &n, TRUE));
    CHECK_EQ(n, 4096);
    CHECK(pattern_ok(buf.data(), 4096, 4096));

    HANDLE port = CreateIoCompletionPort(h, nullptr, 0xC0DE, 0);
    CHECK(port != nullptr);
    std::vector<OVERLAPPED> po(8);
    for (int i = 0; i < 8; i++) {
        memset(&po[i], 0, sizeof po[i]);
        po[i].Offset = (DWORD)(i * kChunk);
        ReadFile(h, &buf[i * kChunk], (DWORD)kChunk, nullptr, &po[i]);
    }
    int got = 0;
    bool keys_ok = true;
    for (int i = 0; i < 8; i++) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED o = nullptr;
        if (GetQueuedCompletionStatus(port, &bytes, &key, &o, 10000)) {
            got++;
            if (key != 0xC0DE || bytes != kChunk || o < &po[0] || o > &po[7]) keys_ok = false;
        }
    }
    CHECK_EQ(got, 8);
    CHECK(keys_ok);
    DWORD bytes;
    ULONG_PTR key;
    LPOVERLAPPED o;
    CHECK(!GetQueuedCompletionStatus(port, &bytes, &key, &o, 20));
    CHECK_EQ(GetLastError(), WAIT_TIMEOUT);
    CHECK(o == nullptr);
    CHECK(PostQueuedCompletionStatus(port, 11, 22, (LPOVERLAPPED)0x30));
    CHECK(PostQueuedCompletionStatus(port, 12, 23, (LPOVERLAPPED)0x40));
    OVERLAPPED_ENTRY ent[4];
    ULONG removed = 0;
    CHECK(GetQueuedCompletionStatusEx(port, ent, 4, &removed, 1000, FALSE));
    CHECK_EQ(removed, 2);
    CHECK_EQ(ent[0].dwNumberOfBytesTransferred, 11);
    CHECK_EQ(ent[1].lpCompletionKey, 23);
    OVERLAPPED skip = {};
    skip.hEvent = (HANDLE)((uintptr_t)CreateEventW(nullptr, TRUE, FALSE, nullptr) | 1);
    ReadFile(h, buf.data(), 16, nullptr, &skip);
    CHECK(GetOverlappedResult(h, &skip, &n, TRUE));
    CHECK(!GetQueuedCompletionStatus(port, &bytes, &key, &o, 50));
    CloseHandle((HANDLE)((uintptr_t)skip.hEvent & ~(uintptr_t)1));
    CloseHandle(port);
    CloseHandle(h);

    h = CreateFileW(u"C:\\pattern.bin", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                    nullptr);
    OVERLAPPED ro = {};
    ro.Offset = 8192;
    g_routine_calls = 0;
    CHECK(ReadFileEx(h, buf.data(), 1024, &ro, io_routine));
    uint64_t t0 = gta6rt_time_monotonic_ns();
    while (g_routine_calls.load() == 0 && t_ms_since(t0) < 5000) SleepEx(100, TRUE);
    CHECK_EQ(g_routine_calls.load(), 1);
    CHECK_EQ(g_routine_bytes.load(), 1024);
    CHECK(pattern_ok(buf.data(), 8192, 1024));
    CHECK(CancelIo(h));
    CloseHandle(h);

    HANDLE w = CreateFileW(u"C:\\owrite.bin", GENERIC_WRITE | GENERIC_READ, 0, nullptr, CREATE_ALWAYS,
                           FILE_FLAG_OVERLAPPED, nullptr);
    OVERLAPPED wo = {};
    wo.Offset = 1 << 20;
    wo.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK(!WriteFile(w, "tail", 4, nullptr, &wo));
    CHECK_EQ(GetLastError(), ERROR_IO_PENDING);
    CHECK(GetOverlappedResult(w, &wo, &n, TRUE));
    CHECK_EQ(n, 4);
    LARGE_INTEGER sz;
    GetFileSizeEx(w, &sz);
    CHECK_EQ(sz.QuadPart, (1 << 20) + 4);
    CloseHandle(wo.hEvent);
    CloseHandle(w);
}

static void test_find() {
    SECTION("FindFirstFile / FindNextFile");
    CHECK(CreateDirectoryW(u"C:\\findme", nullptr));
    CHECK(!CreateDirectoryW(u"C:\\findme", nullptr));
    CHECK_EQ(GetLastError(), ERROR_ALREADY_EXISTS);
    const char16_t* names[] = {u"C:\\findme\\b.txt", u"C:\\findme\\A.txt", u"C:\\findme\\c.dat"};
    for (auto n : names) CloseHandle(CreateFileW(n, GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr));
    CHECK(CreateDirectoryW(u"C:\\findme\\sub", nullptr));
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(u"C:\\findme\\*.txt", &fd);
    CHECK(f != INVALID_HANDLE_VALUE);
    std::vector<std::u16string> seen;
    if (f != INVALID_HANDLE_VALUE) {
        do seen.push_back(fd.cFileName);
        while (FindNextFileW(f, &fd));
        CHECK_EQ(GetLastError(), ERROR_NO_MORE_FILES);
        CHECK(FindClose(f));
    }
    CHECK_EQ(seen.size(), 2);
    CHECK(seen.size() == 2 && seen[0] == u"A.txt" && seen[1] == u"b.txt");

    f = FindFirstFileW(u"C:\\findme\\*", &fd);
    int files = 0, dirs = 0;
    do {
        std::u16string n = fd.cFileName;
        if (n == u"." || n == u"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) dirs++;
        else files++;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    CHECK_EQ(files, 3);
    CHECK_EQ(dirs, 1);

    f = FindFirstFileExW(u"C:\\findme\\*", FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr, 0);
    bool only_dirs = true;
    do
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) only_dirs = false;
    while (FindNextFileW(f, &fd));
    FindClose(f);
    CHECK(only_dirs);

    f = FindFirstFileW(u"C:\\FINDME\\C.DAT", &fd);
    CHECK(f != INVALID_HANDLE_VALUE);
    CHECK(std::u16string(fd.cFileName) == u"c.dat");
    FindClose(f);
    WIN32_FIND_DATAA fa;
    f = FindFirstFileA("C:\\findme\\?.dat", &fa);
    CHECK(f != INVALID_HANDLE_VALUE);
    CHECK(!strcmp(fa.cFileName, "c.dat"));
    FindClose(f);
    CHECK(FindFirstFileW(u"C:\\findme\\*.none", &fd) == INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), ERROR_FILE_NOT_FOUND);
    CHECK(FindFirstFileW(u"C:\\nodir\\*", &fd) == INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), ERROR_PATH_NOT_FOUND);
    CHECK(!FindClose((HANDLE)0x123458));
}

static void test_attributes_and_dirs() {
    SECTION("attributes, delete, move, copy, directories");
    CHECK_EQ(GetFileAttributesW(u"C:\\findme"), FILE_ATTRIBUTE_DIRECTORY);
    CHECK(GetFileAttributesW(u"C:\\findme\\c.dat") & FILE_ATTRIBUTE_ARCHIVE);
    CHECK_EQ(GetFileAttributesW(u"C:\\findme\\none"), INVALID_FILE_ATTRIBUTES);
    CHECK_EQ(GetLastError(), ERROR_FILE_NOT_FOUND);
    CHECK_EQ(GetFileAttributesA("C:\\nodir\\none"), INVALID_FILE_ATTRIBUTES);
    CHECK_EQ(GetLastError(), ERROR_PATH_NOT_FOUND);
    WIN32_FILE_ATTRIBUTE_DATA ad;
    CHECK(GetFileAttributesExW(u"C:\\sync.bin", GetFileExInfoStandard, &ad));
    CHECK_EQ(ad.nFileSizeLow, 300);
    CHECK(SetFileAttributesW(u"C:\\findme\\c.dat", FILE_ATTRIBUTE_READONLY));
    CHECK(GetFileAttributesW(u"C:\\findme\\c.dat") & FILE_ATTRIBUTE_READONLY);
    CHECK(SetFileAttributesW(u"C:\\findme\\c.dat", FILE_ATTRIBUTE_NORMAL));
    CHECK(!(GetFileAttributesW(u"C:\\findme\\c.dat") & FILE_ATTRIBUTE_READONLY));

    CHECK(!MoveFileExW(u"C:\\findme\\c.dat", u"C:\\findme\\b.txt", 0));
    CHECK_EQ(GetLastError(), ERROR_ALREADY_EXISTS);
    CHECK(MoveFileExW(u"C:\\findme\\c.dat", u"C:\\findme\\b.txt", MOVEFILE_REPLACE_EXISTING));
    CHECK_EQ(GetFileAttributesW(u"C:\\findme\\c.dat"), INVALID_FILE_ATTRIBUTES);
    CHECK(MoveFileExW(u"C:\\findme\\b.txt", u"C:\\findme\\moved.txt", 0));
    CHECK(CopyFileW(u"C:\\sync.bin", u"C:\\findme\\copy.bin", TRUE));
    CHECK(!CopyFileW(u"C:\\sync.bin", u"C:\\findme\\copy.bin", TRUE));
    CHECK_EQ(GetLastError(), ERROR_FILE_EXISTS);
    CHECK(GetFileAttributesExW(u"C:\\findme\\copy.bin", GetFileExInfoStandard, &ad));
    CHECK_EQ(ad.nFileSizeLow, 300);
    CHECK(!DeleteFileW(u"C:\\findme\\sub"));
    CHECK_EQ(GetLastError(), ERROR_ACCESS_DENIED);
    CHECK(!RemoveDirectoryW(u"C:\\findme"));
    CHECK_EQ(GetLastError(), ERROR_DIR_NOT_EMPTY);
    CHECK(RemoveDirectoryW(u"C:\\findme\\sub"));
    CHECK(!DeleteFileW(u"C:\\findme\\none"));
    CHECK_EQ(GetLastError(), ERROR_FILE_NOT_FOUND);
    for (auto n : {u"C:\\findme\\A.txt", u"C:\\findme\\moved.txt", u"C:\\findme\\copy.bin"}) CHECK(DeleteFileW(n));
    CHECK(RemoveDirectoryW(u"C:\\findme"));
    CHECK(!CreateDirectoryW(u"C:\\a\\b\\c", nullptr));
    CHECK_EQ(GetLastError(), ERROR_PATH_NOT_FOUND);

    HANDLE h = CreateFileW(u"C:\\temp.bin", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    CHECK(GetFileAttributesW(u"C:\\temp.bin") != INVALID_FILE_ATTRIBUTES);
    CloseHandle(h);
    CHECK_EQ(GetFileAttributesW(u"C:\\temp.bin"), INVALID_FILE_ATTRIBUTES);

    WCHAR buf[1024];
    WCHAR* part = nullptr;
    DWORD n = GetFullPathNameW(u"C:\\x\\.\\y\\..\\z.txt", 1024, buf, &part);
    CHECK(std::u16string(buf, n) == u"C:\\x\\z.txt");
    CHECK(part && std::u16string(part) == u"z.txt");
    CHECK_EQ(GetFullPathNameW(u"C:\\x\\z.txt", 3, buf, nullptr), 11);

    WCHAR old[1024];
    DWORD ol = GetCurrentDirectoryW(1024, old);
    CHECK(ol > 0);
    CHECK(SetCurrentDirectoryW(u"C:\\"));
    n = GetCurrentDirectoryW(1024, buf);
    CHECK(std::u16string(buf, n) == u"C:\\");
    h = CreateFileW(u"rel.bin", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    CloseHandle(h);
    CHECK(GetFileAttributesW(u"C:\\rel.bin") != INVALID_FILE_ATTRIBUTES);
    n = GetFullPathNameW(u"sub\\f.txt", 1024, buf, nullptr);
    CHECK(std::u16string(buf, n) == u"C:\\sub\\f.txt");
    CHECK(SetCurrentDirectoryW(old));
    CHECK(!SetCurrentDirectoryW(u"C:\\nope"));

    ULARGE_INTEGER fr, tot, tf;
    CHECK(GetDiskFreeSpaceExW(u"C:\\", &fr, &tot, &tf));
    CHECK(tot.QuadPart > 0 && fr.QuadPart <= tot.QuadPart);
    n = GetTempPathW(1024, buf);
    CHECK(n > 0 && buf[n - 1] == u'\\');
}

static void test_mappings() {
    SECTION("file mappings");
    HANDLE h = CreateFileW(u"C:\\map.bin", GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    std::vector<char> d(200000);
    for (size_t i = 0; i < d.size(); i++) d[i] = (char)(i * 31);
    DWORD n;
    WriteFile(h, d.data(), (DWORD)d.size(), &n, nullptr);
    HANDLE m = CreateFileMappingW(h, nullptr, PAGE_READWRITE, 0, 0, nullptr);
    CHECK(m != nullptr);
    char* v = (char*)MapViewOfFile(m, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0x10000, 0);
    CHECK(v != nullptr);
    if (v) {
        CHECK(memcmp(v, d.data() + 0x10000, d.size() - 0x10000) == 0);
        MEMORY_BASIC_INFORMATION mbi;
        VirtualQuery(v, &mbi, sizeof mbi);
        CHECK_EQ(mbi.Type, MEM_MAPPED);
        CHECK_EQ(mbi.State, MEM_COMMIT);
        v[0] = 'Q';
        CHECK(FlushViewOfFile(v, 1));
        CHECK(UnmapViewOfFile(v));
        CHECK(!UnmapViewOfFile(v));
    }
    OVERLAPPED o = {};
    o.Offset = 0x10000;
    char c = 0;
    ReadFile(h, &c, 1, &n, &o);
    CHECK_EQ(c, 'Q');
    CHECK(MapViewOfFile(m, FILE_MAP_READ, 0, 100, 0) == nullptr);
    CHECK(MapViewOfFile(m, FILE_MAP_READ, 0, 0, 10000000) == nullptr);
    char* cp = (char*)MapViewOfFile(m, FILE_MAP_COPY, 0, 0, 4096);
    CHECK(cp != nullptr);
    if (cp) {
        cp[1] = 'Z';
        UnmapViewOfFile(cp);
    }
    o.Offset = 1;
    ReadFile(h, &c, 1, &n, &o);
    CHECK_EQ(c, d[1]);
    CloseHandle(m);
    CloseHandle(h);

    HANDLE ro = CreateFileW(u"C:\\map.bin", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    m = CreateFileMappingW(ro, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CHECK(m != nullptr);
    CHECK(MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, 0) == nullptr);
    CHECK_EQ(GetLastError(), ERROR_ACCESS_DENIED);
    CloseHandle(m);
    CHECK(CreateFileMappingW(ro, nullptr, PAGE_READONLY, 0, 300000, nullptr) == nullptr);
    CloseHandle(ro);

    HANDLE named = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 65536, u"gta6rt-test-map");
    CHECK(named != nullptr);
    CHECK_EQ(GetLastError(), ERROR_SUCCESS);
    HANDLE again = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, u"gta6rt-test-map");
    CHECK(again != nullptr);
    char* a = (char*)MapViewOfFile(named, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    char* b = (char*)MapViewOfFile(again, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    CHECK(a && b && a != b);
    if (a && b) {
        a[100] = 42;
        CHECK_EQ(b[100], 42);
        UnmapViewOfFile(a);
        UnmapViewOfFile(b);
    }
    CloseHandle(again);
    CloseHandle(named);
    CHECK(OpenFileMappingW(FILE_MAP_READ, FALSE, u"gta6rt-test-map") == nullptr);
}

static void test_gta6rt_file_api() {
    SECTION("gta6rt_file_* and map_readonly");
    gta6rt_file* f = nullptr;
    CHECK_EQ(gta6rt_file_open("C:\\api.bin", GTA6RT_FILE_READ | GTA6RT_FILE_WRITE | GTA6RT_FILE_CREATE |
                                                 GTA6RT_FILE_TRUNCATE, &f),
             GTA6RT_OK);
    size_t n = 0;
    CHECK_EQ(gta6rt_file_pwrite(f, "hello world", 11, 100, &n), GTA6RT_OK);
    CHECK_EQ(n, 11);
    uint64_t sz = 0;
    CHECK_EQ(gta6rt_file_size(f, &sz), GTA6RT_OK);
    CHECK_EQ(sz, 111);
    char buf[32] = {};
    CHECK_EQ(gta6rt_file_pread(f, buf, 32, 100, &n), GTA6RT_OK);
    CHECK_EQ(n, 11);
    CHECK(!memcmp(buf, "hello world", 11));
    CHECK_EQ(gta6rt_file_truncate(f, 105), GTA6RT_OK);
    CHECK_EQ(gta6rt_file_sync(f), GTA6RT_OK);
    CHECK(gta6rt_file_fd(f) >= 0);
    gta6rt_file_close(f);
    CHECK_EQ(gta6rt_file_open("C:\\api.bin", GTA6RT_FILE_WRITE | GTA6RT_FILE_EXCLUSIVE, &f), GTA6RT_E_ALREADY_EXISTS);
    CHECK_EQ(gta6rt_file_open("C:\\none.bin", GTA6RT_FILE_READ, &f), GTA6RT_E_NOT_FOUND);
    const void* p = nullptr;
    size_t ps = 0;
    CHECK_EQ(gta6rt_file_map_readonly("C:\\api.bin", &p, &ps), GTA6RT_OK);
    CHECK_EQ(ps, 105);
    CHECK(p && !memcmp((const char*)p + 100, "hello", 5));
    gta6rt_file_unmap(p, ps);
    std::string hp = g_tmp + "/root/c/api.bin";
    CHECK_EQ(gta6rt_file_map_readonly(hp.c_str(), &p, &ps), GTA6RT_OK);
    CHECK_EQ(ps, 105);
    gta6rt_file_unmap(p, ps);
}

static void test_std_handles() {
    SECTION("std handles");
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    CHECK(out != INVALID_HANDLE_VALUE && out != nullptr);
    CHECK(out == GetStdHandle(STD_OUTPUT_HANDLE));
    DWORD n = 0;
    CHECK(WriteFile(GetStdHandle(STD_ERROR_HANDLE), "-- (stderr via WriteFile)\n", 26, &n, nullptr));
    CHECK_EQ(n, 26);
    CHECK(GetStdHandle(12345) == INVALID_HANDLE_VALUE);
}

int main() {
    char tmpl[1024];
    const char* td = getenv("TMPDIR");
    snprintf(tmpl, sizeof tmpl, "%s/gta6rt_os_files_XXXXXX", td && *td ? td : "/tmp");
    if (!mkdtemp(tmpl)) {
        perror("mkdtemp");
        return 2;
    }
    char real[1024];
    g_tmp = realpath(tmpl, real) ? real : tmpl;
    test_paths();
    test_create_dispositions();
    test_sync_io();
    test_overlapped();
    test_find();
    test_attributes_and_dirs();
    test_mappings();
    test_gta6rt_file_api();
    test_std_handles();
    std::string cmd = "rm -rf '" + g_tmp + "'";
    if (system(cmd.c_str()) != 0) fprintf(stderr, "cleanup of %s failed\n", g_tmp.c_str());
    return t_finish("os_test_files");
}
