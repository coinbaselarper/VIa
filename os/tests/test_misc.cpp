#include "t.h"

#include <namedpipeapi.h>
#include <psapi.h>
#include <werapi.h>

#include <atomic>
#include <ctime>
#include <string>
#include <strings.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <vector>

static uint64_t ft64(const FILETIME& f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }

static void test_time() {
    SECTION("QPC, tick counts, FILETIME, SYSTEMTIME, Sleep");
    LARGE_INTEGER f, a, b;
    CHECK(QueryPerformanceFrequency(&f));
    CHECK_EQ(f.QuadPart, 10000000);
    CHECK_EQ(gta6rt_time_qpc_frequency(), 10000000);
    QueryPerformanceCounter(&a);
    ULONGLONG t0 = GetTickCount64();
    uint64_t m0 = gta6rt_time_monotonic_ns();
    Sleep(50);
    QueryPerformanceCounter(&b);
    ULONGLONG t1 = GetTickCount64();
    double qms = (double)(b.QuadPart - a.QuadPart) / 1e4;
    CHECK(qms >= 49 && qms < 1000);
    CHECK(t1 - t0 >= 45 && t1 - t0 < 1000);
    CHECK(t_ms_since(m0) >= 49);
    CHECK(GetTickCount() - (DWORD)t1 < 1000);
    bool mono = true;
    LARGE_INTEGER prev;
    QueryPerformanceCounter(&prev);
    for (int i = 0; i < 100000; i++) {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        if (c.QuadPart < prev.QuadPart) mono = false;
        prev = c;
    }
    CHECK(mono);
    ULONGLONG ui = 0;
    CHECK(QueryUnbiasedInterruptTime(&ui));
    CHECK(ui > 0);

    m0 = gta6rt_time_monotonic_ns();
    Sleep(0);
    SleepEx(0, FALSE);
    CHECK(t_ms_since(m0) < 100);
    m0 = gta6rt_time_monotonic_ns();
    Sleep(20);
    CHECK(t_ms_since(m0) >= 19.5);

    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    int64_t unix_from_ft = (int64_t)(ft64(now) / 10000000ull) - 11644473600ll;
    int64_t diff = unix_from_ft - (int64_t)time(nullptr);
    CHECK(diff >= -2 && diff <= 2);
    CHECK(gta6rt_time_filetime_utc() >= ft64(now));
    FILETIME pr;
    GetSystemTimePreciseAsFileTime(&pr);
    CHECK(ft64(pr) >= ft64(now));

    SYSTEMTIME st = {};
    FILETIME ft = {(DWORD)125911584000000000ull, (DWORD)(125911584000000000ull >> 32)};
    CHECK(FileTimeToSystemTime(&ft, &st));
    CHECK_EQ(st.wYear, 2000);
    CHECK_EQ(st.wMonth, 1);
    CHECK_EQ(st.wDay, 1);
    CHECK_EQ(st.wDayOfWeek, 6);
    CHECK_EQ(st.wHour, 0);
    SYSTEMTIME leap = {2024, 2, 0, 29, 13, 45, 30, 123};
    FILETIME lf;
    CHECK(SystemTimeToFileTime(&leap, &lf));
    SYSTEMTIME back;
    CHECK(FileTimeToSystemTime(&lf, &back));
    CHECK_EQ(back.wYear, 2024);
    CHECK_EQ(back.wMonth, 2);
    CHECK_EQ(back.wDay, 29);
    CHECK_EQ(back.wDayOfWeek, 4);
    CHECK_EQ(back.wHour, 13);
    CHECK_EQ(back.wMinute, 45);
    CHECK_EQ(back.wSecond, 30);
    CHECK_EQ(back.wMilliseconds, 123);
    SYSTEMTIME bad = {2023, 2, 0, 29, 0, 0, 0, 0};
    CHECK(!SystemTimeToFileTime(&bad, &lf));
    CHECK_EQ(GetLastError(), ERROR_INVALID_PARAMETER);
    SYSTEMTIME epoch = {1601, 1, 0, 1, 0, 0, 0, 0};
    CHECK(SystemTimeToFileTime(&epoch, &lf));
    CHECK_EQ(ft64(lf), 0);
    SYSTEMTIME unix0 = {1970, 1, 0, 1, 0, 0, 0, 0};
    CHECK(SystemTimeToFileTime(&unix0, &lf));
    CHECK_EQ(ft64(lf), 116444736000000000ull);

    GetSystemTime(&st);
    time_t tt = time(nullptr);
    struct tm g;
    gmtime_r(&tt, &g);
    CHECK_EQ(st.wYear, g.tm_year + 1900);
    CHECK_EQ(st.wMonth, g.tm_mon + 1);
    CHECK_EQ(st.wDayOfWeek, g.tm_wday);
    SYSTEMTIME lt;
    GetLocalTime(&lt);
    struct tm l;
    localtime_r(&tt, &l);
    CHECK_EQ(lt.wHour, l.tm_hour);
    FILETIME loc, utc;
    CHECK(FileTimeToLocalFileTime(&now, &loc));
    CHECK(LocalFileTimeToFileTime(&loc, &utc));
    CHECK_EQ(ft64(utc), ft64(now));
    CHECK_EQ(CompareFileTime(&now, &pr), ft64(now) < ft64(pr) ? -1 : 0);
    CHECK_EQ(CompareFileTime(&now, &now), 0);
}

static void test_random() {
    SECTION("random bytes");
    std::vector<uint8_t> a(65536), b(65536);
    CHECK_EQ(gta6rt_random_bytes(a.data(), a.size()), GTA6RT_OK);
    CHECK_EQ(gta6rt_random_bytes(b.data(), b.size()), GTA6RT_OK);
    CHECK(a != b);
    int hist[256] = {};
    for (uint8_t v : a) hist[v]++;
    int lo = 1 << 30, hi = 0;
    for (int h : hist) lo = h < lo ? h : lo, hi = h > hi ? h : hi;
    CHECK(lo > 150 && hi < 380);
    CHECK_EQ(gta6rt_random_bytes(nullptr, 0), GTA6RT_OK);
    CHECK_EQ(gta6rt_random_bytes(nullptr, 4), GTA6RT_E_INVALID_ARG);
}

static void test_env() {
    SECTION("environment, command line");
    CHECK(SetEnvironmentVariableW(u"Gta6rt_Test_Var", u"value-1"));
    WCHAR buf[64];
    CHECK_EQ(GetEnvironmentVariableW(u"GTA6RT_TEST_VAR", buf, 64), 7);
    CHECK(std::u16string(buf) == u"value-1");
    CHECK_EQ(GetEnvironmentVariableW(u"gta6rt_test_var", buf, 3), 8);
    char abuf[64];
    CHECK_EQ(GetEnvironmentVariableA("gta6rt_test_var", abuf, 64), 7);
    CHECK(!strcmp(abuf, "value-1"));
    CHECK(gta6rt_env_get("GTA6RT_TEST_VAR") && !strcmp(gta6rt_env_get("GTA6RT_TEST_VAR"), "value-1"));
    CHECK(getenv("Gta6rt_Test_Var") && !strcmp(getenv("Gta6rt_Test_Var"), "value-1"));
    CHECK_EQ(ExpandEnvironmentStringsW(u"a-%GTA6RT_TEST_VAR%-%NOPE_NOT_SET%-b", buf, 64), 27);
    CHECK(std::u16string(buf) == u"a-value-1-%NOPE_NOT_SET%-b");
    LPWCH block = GetEnvironmentStringsW();
    bool found = false;
    for (LPWCH p = block; *p; p += std::u16string(p).size() + 1)
        if (std::u16string(p) == u"Gta6rt_Test_Var=value-1") found = true;
    CHECK(found);
    FreeEnvironmentStringsW(block);
    CHECK(SetEnvironmentVariableW(u"GTA6RT_TEST_VAR", nullptr));
    CHECK_EQ(GetEnvironmentVariableW(u"GTA6RT_TEST_VAR", buf, 64), 0);
    CHECK_EQ(GetLastError(), ERROR_ENVVAR_NOT_FOUND);
    CHECK(gta6rt_env_get("GTA6RT_TEST_VAR") == nullptr);
    CHECK_EQ(gta6rt_env_set("A=B", "x"), GTA6RT_E_INVALID_ARG);

    CHECK(strstr(GetCommandLineA(), "os_test_misc") != nullptr);
    CHECK_EQ(gta6rt_process_set_command_line("game.exe -arg \"x y\""), GTA6RT_OK);
    CHECK(!strcmp(GetCommandLineA(), "game.exe -arg \"x y\""));
    CHECK(std::u16string(GetCommandLineW()) == u"game.exe -arg \"x y\"");
}

static std::atomic<int> g_closed;
static void on_close(void* p) { g_closed += (int)(uintptr_t)p; }

static DWORD WINAPI set_err(LPVOID) {
    SetLastError(1234);
    return GetLastError();
}

static void test_handles() {
    SECTION("handle table, last error");
    uint64_t h1 = 0, h2 = 0;
    CHECK_EQ(gta6rt_handle_alloc(GTA6RT_HANDLE_OTHER, (void*)3, on_close, &h1), GTA6RT_OK);
    CHECK_EQ(gta6rt_handle_alloc(GTA6RT_HANDLE_FILE, (void*)5, on_close, &h2), GTA6RT_OK);
    CHECK(h1 && h2 && h1 != h2);
    CHECK_EQ(h1 & 3, 0);
    CHECK(h1 < 0x100000000ull);
    CHECK(gta6rt_handle_lookup(h1, GTA6RT_HANDLE_OTHER) == (void*)3);
    CHECK(gta6rt_handle_lookup(h1, GTA6RT_HANDLE_NONE) == (void*)3);
    CHECK(gta6rt_handle_lookup(h1, GTA6RT_HANDLE_FILE) == nullptr);
    CHECK(gta6rt_handle_lookup(h1 + 1, GTA6RT_HANDLE_NONE) == nullptr);
    HANDLE d = nullptr;
    CHECK(DuplicateHandle(GetCurrentProcess(), (HANDLE)h1, GetCurrentProcess(), &d, 0, FALSE, DUPLICATE_SAME_ACCESS));
    CHECK(CompareObjectHandles((HANDLE)h1, d));
    CHECK(!CompareObjectHandles((HANDLE)h1, (HANDLE)h2));
    CHECK_EQ(gta6rt_handle_close(h1), GTA6RT_OK);
    CHECK_EQ(g_closed.load(), 0);
    CHECK(gta6rt_handle_lookup((uint64_t)d, GTA6RT_HANDLE_OTHER) == (void*)3);
    CHECK(CloseHandle(d));
    CHECK_EQ(g_closed.load(), 3);
    CHECK_EQ(gta6rt_handle_close(h1), GTA6RT_E_INVALID_ARG);
    uint64_t h3 = 0;
    gta6rt_handle_alloc(GTA6RT_HANDLE_OTHER, nullptr, nullptr, &h3);
    CHECK(h3 == h1 || h3 == (uint64_t)d);
    gta6rt_handle_close(h3);

    CHECK(SetHandleInformation((HANDLE)h2, HANDLE_FLAG_PROTECT_FROM_CLOSE, HANDLE_FLAG_PROTECT_FROM_CLOSE));
    DWORD fl = 0;
    CHECK(GetHandleInformation((HANDLE)h2, &fl));
    CHECK_EQ(fl, HANDLE_FLAG_PROTECT_FROM_CLOSE);
    CHECK(!CloseHandle((HANDLE)h2));
    CHECK(SetHandleInformation((HANDLE)h2, HANDLE_FLAG_PROTECT_FROM_CLOSE, 0));
    CHECK(CloseHandle((HANDLE)h2));
    CHECK_EQ(g_closed.load(), 8);
    CHECK(!CloseHandle((HANDLE)0x7FFFFFF0));
    CHECK_EQ(GetLastError(), ERROR_INVALID_HANDLE);
    CHECK(!CloseHandle(nullptr));
    CHECK(CloseHandle(GetCurrentProcess()));
    CHECK(CloseHandle(GetCurrentThread()));

    std::vector<HANDLE> evs;
    for (int i = 0; i < 20000; i++) evs.push_back(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    bool all = true;
    for (HANDLE e : evs) all = all && e && ((uintptr_t)e & 3) == 0 && (uintptr_t)e < 0x100000000ull;
    CHECK(all);
    for (HANDLE e : evs) CloseHandle(e);

    SetLastError(42);
    HANDLE t = CreateThread(nullptr, 0, set_err, nullptr, 0, nullptr);
    WaitForSingleObject(t, 10000);
    DWORD c = 0;
    GetExitCodeThread(t, &c);
    CloseHandle(t);
    CHECK_EQ(c, 1234);
    SetLastError(42);
    CHECK_EQ(GetLastError(), 42);
    CHECK_EQ(gta6rt_last_error_get(), 42);
    gta6rt_last_error_set(7);
    CHECK_EQ(GetLastError(), 7);
}

static void test_strings() {
    SECTION("code pages and string compares");
    const char* utf8 = "A\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80";
    CHECK_EQ(MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0), 6);
    WCHAR w[16];
    CHECK_EQ(MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 16), 6);
    CHECK(w[0] == u'A' && w[1] == 0xE9 && w[2] == 0x20AC && w[3] == 0xD83D && w[4] == 0xDE00 && w[5] == 0);
    CHECK_EQ(MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 3), 0);
    CHECK_EQ(GetLastError(), ERROR_INSUFFICIENT_BUFFER);
    CHECK_EQ(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, "a\xff", 2, w, 16), 0);
    CHECK_EQ(GetLastError(), ERROR_NO_UNICODE_TRANSLATION);
    char out[32];
    CHECK_EQ(WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr), 11);
    CHECK_EQ(WideCharToMultiByte(CP_UTF8, 0, w, -1, out, 32, nullptr, nullptr), 11);
    CHECK(!strcmp(out, utf8));
    WCHAR lone[2] = {0xD800, 0};
    CHECK_EQ(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, lone, 1, out, 32, nullptr, nullptr), 0);
    CHECK_EQ(MultiByteToWideChar(1252, 0, "\x80\xe9", 2, w, 16), 2);
    CHECK(w[0] == 0x20AC && w[1] == 0xE9);
    BOOL used = FALSE;
    WCHAR cjk[2] = {0x4E2D, 0x20AC};
    CHECK_EQ(WideCharToMultiByte(1252, 0, cjk, 2, out, 32, nullptr, &used), 2);
    CHECK(used);
    CHECK(out[0] == '?' && (unsigned char)out[1] == 0x80);
    CHECK_EQ(CompareStringOrdinal(u"Hello", -1, u"hello", -1, TRUE), CSTR_EQUAL);
    CHECK_EQ(CompareStringOrdinal(u"Hello", -1, u"hello", -1, FALSE), CSTR_LESS_THAN);
    CHECK_EQ(CompareStringOrdinal(u"abc", 2, u"abd", 2, FALSE), CSTR_EQUAL);
    CHECK_EQ(lstrlenW(u"four"), 4);
    CHECK_EQ(lstrcmpiW(u"ABC", u"abc"), 0);
    WCHAR up[8];
    CHECK_EQ(LCMapStringEx(nullptr, LCMAP_UPPERCASE, u"ab\u00e9", 3, up, 8, nullptr, nullptr, 0), 3);
    CHECK(up[0] == u'A' && up[1] == u'B' && up[2] == 0xC9);
    CHECK_EQ(GetACP(), CP_UTF8);
}

static void test_sysinfo() {
    SECTION("system information");
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    CHECK_EQ(si.dwPageSize, 4096);
    CHECK_EQ(si.dwAllocationGranularity, 65536);
    CHECK(si.dwNumberOfProcessors >= 1 && si.dwNumberOfProcessors <= 64);
    CHECK_EQ(si.wProcessorArchitecture, PROCESSOR_ARCHITECTURE_AMD64);
    CHECK_EQ(GetActiveProcessorCount(0), si.dwNumberOfProcessors);
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof ms;
    CHECK(GlobalMemoryStatusEx(&ms));
    uint64_t mem = 0;
    size_t len = sizeof mem;
    sysctlbyname("hw.memsize", &mem, &len, nullptr, 0);
    CHECK_EQ(ms.ullTotalPhys, mem);
    CHECK(ms.ullAvailPhys > 0 && ms.ullAvailPhys <= ms.ullTotalPhys);
    DWORD need = 0;
    CHECK(!GetLogicalProcessorInformation(nullptr, &need));
    CHECK_EQ(GetLastError(), ERROR_INSUFFICIENT_BUFFER);
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> lpi(need / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    CHECK(GetLogicalProcessorInformation(lpi.data(), &need));
    int cores = 0;
    for (auto& e : lpi) cores += e.Relationship == RelationProcessorCore;
    CHECK_EQ(cores, (int)si.dwNumberOfProcessors);
    CHECK(IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE));
    OSVERSIONINFOEXW vi = {};
    vi.dwOSVersionInfoSize = sizeof vi;
    CHECK(GetVersionExW(&vi));
    CHECK_EQ(vi.dwMajorVersion, 10);
    CHECK(GetCurrentProcessId() == (DWORD)getpid());
    FILETIME c, e, k, u;
    CHECK(GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u));
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    CHECK(ft64(c) > 0 && ft64(c) <= ft64(now));
}

static gta6rt_gva fake_export(void* user, const char* name, uint32_t ordinal) {
    if (name && !strcmp(name, "Foo")) return (gta6rt_gva)(uintptr_t)user + 0x10;
    if (!name && ordinal == 7) return (gta6rt_gva)(uintptr_t)user + 0x70;
    return 0;
}

static std::atomic<int> g_load_hook_calls;
static gta6rt_module* load_hook(const char* name, uint32_t, void*) {
    g_load_hook_calls++;
    if (strcasecmp(name, "late.dll") != 0) return nullptr;
    gta6rt_module_desc d = {"late.dll", nullptr, GTA6RT_MODULE_HOST_SHIM, 0x7000000, 0x1000, fake_export, (void*)0x7000000};
    gta6rt_module* m = nullptr;
    gta6rt_module_register(&d, &m);
    return m;
}

static void test_modules() {
    SECTION("module table, GetModuleHandle, GetProcAddress, LoadLibrary");
    gta6rt_module_desc k = {"KERNEL32.dll", nullptr, GTA6RT_MODULE_HOST_SHIM, 0x6000000, 0x10000, fake_export,
                            (void*)0x6000000};
    gta6rt_module* km = nullptr;
    CHECK_EQ(gta6rt_module_register(&k, &km), GTA6RT_OK);
    CHECK_EQ(gta6rt_module_register(&k, nullptr), GTA6RT_E_ALREADY_EXISTS);
    gta6rt_module_desc g = {"game_scarlett_master.exe", "/tmp/game/game_scarlett_master.exe", GTA6RT_MODULE_GUEST_IMAGE,
                            0x140000000ull, 0xB06D000, nullptr, nullptr};
    gta6rt_module* gm = nullptr;
    CHECK_EQ(gta6rt_module_register(&g, &gm), GTA6RT_OK);
    CHECK(gta6rt_module_find_by_name("kernel32") == km);
    CHECK(gta6rt_module_find_by_name("Kernel32.DLL") == km);
    CHECK(gta6rt_module_find_by_name("C:\\Windows\\System32\\kernel32.dll") == km);
    CHECK(gta6rt_module_find_by_address(0x6000123) == km);
    CHECK(gta6rt_module_find_by_address(0x6010000) == nullptr);
    CHECK(gta6rt_module_find_by_base(0x140000000ull) == gm);
    CHECK(gta6rt_module_at(0) == km || gta6rt_module_at(1) == km);
    CHECK(GetModuleHandleW(u"kernel32.dll") == (HMODULE)0x6000000);
    CHECK(GetModuleHandleA(nullptr) == (HMODULE)0x140000000ull);
    CHECK(GetModuleHandleA("nothere.dll") == nullptr);
    CHECK_EQ(GetLastError(), ERROR_MOD_NOT_FOUND);
    HMODULE hm = nullptr;
    CHECK(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)(uintptr_t)0x140001000ull, &hm));
    CHECK(hm == (HMODULE)0x140000000ull);
    CHECK(GetProcAddress((HMODULE)0x6000000, "Foo") == (FARPROC)0x6000010);
    CHECK(GetProcAddress((HMODULE)0x6000000, (LPCSTR)(uintptr_t)7) == (FARPROC)0x6000070);
    CHECK(GetProcAddress((HMODULE)0x6000000, "Bar") == nullptr);
    CHECK_EQ(GetLastError(), ERROR_PROC_NOT_FOUND);
    CHECK(GetProcAddress((HMODULE)0x1234, "Foo") == nullptr);
    CHECK_EQ(GetLastError(), ERROR_MOD_NOT_FOUND);
    CHECK(LoadLibraryA("KERNEL32") == (HMODULE)0x6000000);
    CHECK(FreeLibrary((HMODULE)0x6000000));
    CHECK(LoadLibraryW(u"missing.dll") == nullptr);
    CHECK_EQ(GetLastError(), ERROR_MOD_NOT_FOUND);
    gta6rt_module_set_load_hook(load_hook, nullptr);
    CHECK(LoadLibraryExW(u"LATE.DLL", nullptr, 0) == (HMODULE)0x7000000);
    CHECK(LoadLibraryA("late") == (HMODULE)0x7000000);
    CHECK_EQ(g_load_hook_calls.load(), 1);
    gta6rt_module_set_load_hook(nullptr, nullptr);
    WCHAR fn[512];
    DWORD n = GetModuleFileNameW(nullptr, fn, 512);
    CHECK(std::u16string(fn, n) == u"Z:\\tmp\\game\\game_scarlett_master.exe");
    CHECK_EQ(GetModuleFileNameW(nullptr, fn, 5), 5);
    CHECK_EQ(GetLastError(), ERROR_INSUFFICIENT_BUFFER);
    char fa[512];
    CHECK(GetModuleFileNameA((HMODULE)0x6000000, fa, 512) > 0);
    CHECK(!strcmp(fa, "KERNEL32.dll"));
    CHECK(gta6rt_module_info(km)->base == 0x6000000);
    CHECK_EQ(gta6rt_module_get_export(km, "Foo", 0), 0x6000010);
    MODULEINFO mi;
    CHECK(GetModuleInformation(GetCurrentProcess(), (HMODULE)0x140000000ull, &mi, sizeof mi));
    CHECK(mi.lpBaseOfDll == (LPVOID)0x140000000ull && mi.SizeOfImage == 0xB06D000);
    HMODULE mods[16];
    DWORD need = 0;
    CHECK(EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need));
    CHECK(need >= 3 * sizeof(HMODULE));
    CHECK(mods[0] == (HMODULE)0x140000000ull);
    PROCESS_MEMORY_COUNTERS_EX pmc;
    CHECK(GetProcessMemoryInfo(GetCurrentProcess(), (PPROCESS_MEMORY_COUNTERS)&pmc, sizeof pmc));
    CHECK(pmc.WorkingSetSize > (1u << 20) && pmc.PeakWorkingSetSize >= pmc.WorkingSetSize);
    CHECK(pmc.PrivateUsage > 0);
    gta6rt_module_set_main(km);
    CHECK(GetModuleHandleW(nullptr) == (HMODULE)0x6000000);
    CHECK_EQ(gta6rt_module_unregister(km), GTA6RT_OK);
    CHECK(GetModuleHandleW(nullptr) == (HMODULE)0x140000000ull);
    CHECK_EQ(gta6rt_module_unregister(km), GTA6RT_E_NOT_FOUND);
}

static void test_logging() {
    SECTION("status names, logging");
    CHECK(!strcmp(gta6rt_status_name(GTA6RT_OK), "ok"));
    CHECK(!strcmp(gta6rt_status_name(GTA6RT_E_TIMEOUT), "timeout"));
    CHECK(!strcmp(gta6rt_status_name((gta6rt_status)-999), "unknown"));
    gta6rt_log(GTA6RT_LOG_INFO, "test", "log line %d", 1);
    gta6rt_log(GTA6RT_LOG_TRACE, "test", "suppressed at default level");
    gta6rt_log_signal_safe("-- (signal-safe log line)\n");
    OutputDebugStringA("-- OutputDebugString line\n");
    CHECK(EncodePointer((PVOID)0x1234) == (PVOID)0x1234);
    CHECK(SetProcessPriorityBoost(GetCurrentProcess(), TRUE));
    BOOL boost = FALSE;
    CHECK(GetProcessPriorityBoost(GetCurrentProcess(), &boost));
    CHECK(boost);
    CHECK(!WaitNamedPipeW(u"\\\\.\\pipe\\x", 0));
    CHECK_EQ(GetLastError(), ERROR_FILE_NOT_FOUND);
    CHECK(!SetNamedPipeHandleState((HANDLE)0x1000, nullptr, nullptr, nullptr));
    CHECK_EQ(WerRegisterFile(u"C:\\log.txt", WerRegFileTypeOther, 0), S_OK);
}

int main() {
    test_time();
    test_random();
    test_env();
    test_handles();
    test_strings();
    test_sysinfo();
    test_modules();
    test_logging();
    return t_finish("os_test_misc");
}
