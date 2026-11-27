#include <cstdint>
#include <cstring>
#include <string>

#include <gta6rt/os.h>

#include "process_shim.h"
#include "shim_call.h"
#include "test.h"

using tcall::call;

namespace {
constexpr const char* kK = "kernel32.dll";
struct FileTime {
    uint32_t lo, hi;
};
struct SysTime {
    uint16_t year, month, dow, day, hour, minute, second, ms;
};
uint64_t u64(const FileTime& f) { return (uint64_t{f.hi} << 32) | f.lo; }
}  // namespace

TEST(k32_last_error_and_identity) {
    call<void>(kK, "SetLastError", 1234u);
    CHECK_EQ(call<uint32_t>(kK, "GetLastError"), 1234u);
    CHECK_EQ(call<intptr_t>(kK, "GetCurrentProcess"), -1);
    CHECK(call<uint32_t>(kK, "GetCurrentProcessId") != 0);
    CHECK(call<uint32_t>(kK, "GetCurrentThreadId") != 0);
    CHECK_EQ(call<uint32_t>(kK, "GetProcessId", reinterpret_cast<void*>(static_cast<intptr_t>(-1))), call<uint32_t>(kK, "GetCurrentProcessId"));
    CHECK_EQ(call<uint32_t>(kK, "GetProcessId", reinterpret_cast<void*>(0x1234)), 0u);
    CHECK_EQ(call<uint32_t>(kK, "GetLastError"), 6u);
}

TEST(k32_file_time_system_time_roundtrip) {
    SysTime st{2024, 2, 0, 29, 13, 45, 56, 789};
    FileTime ft{};
    CHECK_EQ(call<int>(kK, "SystemTimeToFileTime", &st, &ft), 1);
    CHECK_EQ(u64(ft), 133536879567890000ull);
    SysTime back{};
    CHECK_EQ(call<int>(kK, "FileTimeToSystemTime", &ft, &back), 1);
    CHECK(back.year == 2024 && back.month == 2 && back.day == 29 && back.hour == 13 && back.minute == 45 &&
          back.second == 56 && back.ms == 789);
    CHECK_EQ(back.dow, 4);
    SysTime epoch{1601, 1, 0, 1, 0, 0, 0, 0};
    CHECK_EQ(call<int>(kK, "SystemTimeToFileTime", &epoch, &ft), 1);
    CHECK_EQ(u64(ft), 0u);
    SysTime bad{2024, 13, 0, 1, 0, 0, 0, 0};
    CHECK_EQ(call<int>(kK, "SystemTimeToFileTime", &bad, &ft), 0);
    CHECK_EQ(call<uint32_t>(kK, "GetLastError"), 87u);
}

TEST(k32_local_file_time_inverts) {
    FileTime utc{0xD53E8000u, 0x019DB1DEu}, local{}, back{};
    CHECK_EQ(call<int>(kK, "FileTimeToLocalFileTime", &utc, &local), 1);
    CHECK_EQ(call<int>(kK, "LocalFileTimeToFileTime", &local, &back), 1);
    CHECK_EQ(u64(back), u64(utc));
}

TEST(k32_clock_functions) {
    FileTime a{}, b{};
    call<void>(kK, "GetSystemTimeAsFileTime", &a);
    call<void>(kK, "GetSystemTimePreciseAsFileTime", &b);
    CHECK(u64(a) > 132000000000000000ull);
    CHECK(u64(b) >= u64(a));
    SysTime st{};
    call<void>(kK, "GetSystemTime", &st);
    CHECK(st.year >= 2024 && st.month >= 1 && st.month <= 12);
    call<void>(kK, "GetLocalTime", &st);
    CHECK(st.year >= 2024);
    const uint64_t t0 = call<uint64_t>(kK, "GetTickCount64");
    call<void>(kK, "Sleep", 20u);
    CHECK(call<uint64_t>(kK, "GetTickCount64") >= t0 + 15);
    int64_t freq = 0, c1 = 0, c2 = 0;
    CHECK_EQ(call<int>(kK, "QueryPerformanceFrequency", &freq), 1);
    CHECK_EQ(freq, 10000000);
    CHECK_EQ(call<int>(kK, "QueryPerformanceCounter", &c1), 1);
    call<void>(kK, "Sleep", 5u);
    CHECK_EQ(call<int>(kK, "QueryPerformanceCounter", &c2), 1);
    CHECK(c2 > c1);
}

TEST(k32_multibyte_wide_conversions) {
    char16_t wide[16];
    CHECK_EQ(call<int>(kK, "MultiByteToWideChar", 65001u, 0u, "h\xc3\xa9llo", -1, wide, 16), 6);
    CHECK(wide[0] == u'h' && wide[1] == 0xe9 && wide[5] == 0);
    CHECK_EQ(call<int>(kK, "MultiByteToWideChar", 65001u, 0u, "h\xc3\xa9llo", -1, static_cast<char16_t*>(nullptr), 0), 6);
    CHECK_EQ(call<int>(kK, "MultiByteToWideChar", 65001u, 0u, "abc", 3, wide, 16), 3);
    CHECK_EQ(call<int>(kK, "MultiByteToWideChar", 65001u, 0u, "hello", -1, wide, 3), 0);
    CHECK_EQ(call<uint32_t>(kK, "GetLastError"), 122u);
    CHECK_EQ(call<int>(kK, "MultiByteToWideChar", 65001u, 8u, "\xff\xfe", -1, wide, 16), 0);
    CHECK_EQ(call<uint32_t>(kK, "GetLastError"), 1113u);
    CHECK_EQ(call<int>(kK, "MultiByteToWideChar", 65001u, 0u, "\xf0\x9f\x98\x80", -1, wide, 16), 3);
    CHECK(wide[0] == 0xD83D && wide[1] == 0xDE00);
    CHECK_EQ(call<int>(kK, "MultiByteToWideChar", 1252u, 0u, "\x80", 1, wide, 16), 1);
    CHECK_EQ(wide[0], 0x20AC);

    char out[16];
    CHECK_EQ(call<int>(kK, "WideCharToMultiByte", 65001u, 0u, u"h\u00e9", -1, out, 16, static_cast<const char*>(nullptr), static_cast<int*>(nullptr)), 4);
    CHECK(!std::strcmp(out, "h\xc3\xa9"));
    CHECK_EQ(call<int>(kK, "WideCharToMultiByte", 65001u, 0u, u"abc", -1, static_cast<char*>(nullptr), 0, static_cast<const char*>(nullptr), static_cast<int*>(nullptr)), 4);
    CHECK_EQ(call<int>(kK, "WideCharToMultiByte", 65001u, 0u, u"\U0001F600", -1, out, 16, static_cast<const char*>(nullptr), static_cast<int*>(nullptr)), 5);
    const char16_t lone[] = {0xD800, 0};
    CHECK_EQ(call<int>(kK, "WideCharToMultiByte", 65001u, 0x80u, lone, -1, out, 16, static_cast<const char*>(nullptr), static_cast<int*>(nullptr)), 0);
    CHECK_EQ(call<uint32_t>(kK, "GetLastError"), 1113u);
    int used = 0;
    CHECK_EQ(call<int>(kK, "WideCharToMultiByte", 1252u, 0u, u"a\u4e2d", -1, out, 16, static_cast<const char*>(nullptr), &used), 3);
    CHECK(out[1] == '?' && used == 1);
    CHECK_EQ(call<int>(kK, "WideCharToMultiByte", 65001u, 0u, u"a", -1, out, 16, "?", static_cast<int*>(nullptr)), 0);
    CHECK_EQ(call<uint32_t>(kK, "GetLastError"), 87u);
}

TEST(k32_lstrcmpw_and_locale) {
    CHECK_EQ(call<int>(kK, "lstrcmpW", u"abc", u"abd"), -1);
    CHECK_EQ(call<int>(kK, "lstrcmpW", u"abc", u"abc"), 0);
    char16_t name[16];
    CHECK_EQ(call<int>(kK, "GetUserDefaultLocaleName", name, 16), 6);
    CHECK(name[0] == u'e' && name[2] == u'-');
    CHECK_EQ(call<int>(kK, "GetUserDefaultLocaleName", name, 3), 0);
    CHECK_EQ(call<int>(kK, "GetUserDefaultGeoName", name, 16), 3);
    CHECK_EQ(call<uint32_t>(kK, "LocaleNameToLCID", u"en-US", 0u), 0x0409u);
    CHECK_EQ(call<uint32_t>(kK, "LocaleNameToLCID", u"ja-JP", 0u), 0x0411u);
    CHECK_EQ(call<uint32_t>(kK, "LocaleNameToLCID", u"xx-XX", 0u), 0u);
}

TEST(k32_date_time_format) {
    SysTime st{2024, 3, 5, 15, 9, 7, 3, 0};
    char16_t buf[64];
    CHECK_EQ(call<int>(kK, "GetDateFormatEx", u"en-US", 0u, &st, static_cast<const char16_t*>(nullptr), buf, 64, static_cast<const char16_t*>(nullptr)), 10);
    CHECK(std::u16string(buf) == u"3/15/2024");
    CHECK_EQ(call<int>(kK, "GetDateFormatEx", u"en-US", 0u, &st, u"dddd, MMMM dd, yyyy", buf, 64, static_cast<const char16_t*>(nullptr)), 23);
    CHECK(std::u16string(buf) == u"Friday, March 15, 2024");
    CHECK_EQ(call<int>(kK, "GetTimeFormatEx", u"en-US", 0u, &st, static_cast<const char16_t*>(nullptr), buf, 64), 11);
    CHECK(std::u16string(buf) == u"9:07:03 AM");
    CHECK_EQ(call<int>(kK, "GetTimeFormatEx", u"en-US", 0u, &st, u"HH':'mm", buf, 64), 6);
    CHECK(std::u16string(buf) == u"09:07");
    CHECK_EQ(call<int>(kK, "GetTimeFormatEx", u"en-US", 0u, &st, static_cast<const char16_t*>(nullptr), buf, 4), 0);
    CHECK_EQ(call<uint32_t>(kK, "GetLastError"), 122u);
}

TEST(k32_system_info) {
    struct Ms {
        uint32_t len, load;
        uint64_t phys, avail_phys, page, avail_page, virt, avail_virt, ext;
    } ms{};
    ms.len = sizeof ms;
    CHECK_EQ(call<int>(kK, "GlobalMemoryStatusEx", &ms), 1);
    CHECK(ms.phys >= (1ull << 30) && ms.avail_phys <= ms.phys && ms.load <= 100);
    ms.len = 8;
    CHECK_EQ(call<int>(kK, "GlobalMemoryStatusEx", &ms), 0);

    uint32_t len = 0;
    CHECK_EQ(call<int>(kK, "GetLogicalProcessorInformation", static_cast<void*>(nullptr), &len), 0);
    CHECK_EQ(call<uint32_t>(kK, "GetLastError"), 122u);
    CHECK(len >= 64 && len % 32 == 0);
    std::string buf(len, '\0');
    CHECK_EQ(call<int>(kK, "GetLogicalProcessorInformation", buf.data(), &len), 1);
    uint32_t core_entries = 0, rel0;
    std::memcpy(&rel0, buf.data() + 8, 4);
    CHECK_EQ(rel0, 0u);
    for (uint32_t off = 0; off < len; off += 32) {
        uint32_t rel;
        std::memcpy(&rel, buf.data() + off + 8, 4);
        if (rel == 0) ++core_entries;
    }
    CHECK(core_entries >= 1);

    CHECK_EQ(call<int>(kK, "IsProcessorFeaturePresent", 10u), 1);
    CHECK_EQ(call<int>(kK, "IsProcessorFeaturePresent", 40u), 1);
    CHECK_EQ(call<int>(kK, "IsProcessorFeaturePresent", 41u), 0);

    uintptr_t low = 0, high = 0;
    call<void>(kK, "GetCurrentThreadStackLimits", &low, &high);
    uintptr_t here = reinterpret_cast<uintptr_t>(&low);
    CHECK(low < here && here <= high);

    uint8_t slist[16];
    std::memset(slist, 0xAB, sizeof slist);
    call<void>(kK, "InitializeSListHead", slist);
    CHECK_EQ(slist[0] | slist[15], 0);
}

TEST(k32_message_and_misc) {
    char* msg = nullptr;
    CHECK(call<uint32_t>(kK, "FormatMessageA", 0x1100u, static_cast<const void*>(nullptr), 2u, 0u, reinterpret_cast<char*>(&msg), 0u, static_cast<void*>(nullptr)) > 10);
    CHECK(msg && std::strstr(msg, "cannot find the file"));
    CHECK(call<void*>(kK, "LocalFree", msg) == nullptr);
    char small[8];
    CHECK_EQ(call<uint32_t>(kK, "FormatMessageA", 0x1000u, static_cast<const void*>(nullptr), 2u, 0u, small, 8u, static_cast<void*>(nullptr)), 0u);
    CHECK_EQ(call<int>(kK, "WaitNamedPipeW", u"\\\\.\\pipe\\x", 100u), 0);
    CHECK_EQ(call<uint32_t>(kK, "GetLastError"), 2u);
    CHECK_EQ(call<int32_t>(kK, "WerRegisterFile", u"x.txt", 1, 0u), 0);
    void* reg = nullptr;
    CHECK_EQ(call<uint32_t>(kK, "RegisterAppConstrainedChangeNotification", reinterpret_cast<void*>(0x1000), static_cast<void*>(nullptr), &reg), 0u);
    CHECK(reg != nullptr);
    call<void>(kK, "UnregisterAppConstrainedChangeNotification", reg);
    CHECK_EQ(call<int>(kK, "SetProcessPriorityBoost", reinterpret_cast<void*>(static_cast<intptr_t>(-1)), 1), 1);
    uint8_t group[16] = {};
    CHECK_EQ(call<int>(kK, "GetThreadGroupAffinity", reinterpret_cast<void*>(static_cast<intptr_t>(-2)), group), 0);
}

TEST(k32_disk_free_space) {
    uint64_t avail = 0, total = 0, free_bytes = 0;
    CHECK_EQ(call<int>(kK, "GetDiskFreeSpaceExW", u"/", &avail, &total, &free_bytes), 1);
    CHECK(total > 0 && avail <= total && free_bytes <= total);
}

TEST(process_command_line_and_exit_code) {
    rt::process::set_command_line(u"G:\\game.exe", {u"-a", u"two words", u"x\"y"});
    char16_t* w = call<char16_t*>(kK, "GetCommandLineW");
    CHECK(std::u16string(w) == u"G:\\game.exe -a \"two words\" \"x\\\"y\"");
    char* a = call<char*>(kK, "GetCommandLineA");
    CHECK(std::string(a) == "G:\\game.exe -a \"two words\" \"x\\\"y\"");
    CHECK_EQ(call<int>("ucrtbase.dll", "_configure_narrow_argv", 1), 0);
    int* argc = call<int*>("ucrtbase.dll", "__p___argc");
    char*** argv = call<char***>("ucrtbase.dll", "__p___argv");
    CHECK_EQ(*argc, 4);
    CHECK(std::string((*argv)[2]) == "two words");
    CHECK((*argv)[4] == nullptr);
    CHECK_EQ(call<int>("ucrtbase.dll", "_configure_wide_argv", 1), 0);
    char16_t*** wargv = call<char16_t***>("ucrtbase.dll", "__p___wargv");
    CHECK(std::u16string((*wargv)[3]) == u"x\"y");
    char* winmain = call<char*>("ucrtbase.dll", "_get_narrow_winmain_command_line");
    CHECK(std::string(winmain) == "-a \"two words\" \"x\\\"y\"");
}
