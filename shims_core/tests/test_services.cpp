#include <objbase.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include <gta6rt/os.h>

#include "bcrypt_hooks.h"
#include "com_shim.h"
#include "console_registry.h"
#include "lifecycle.h"
#include "shim_call.h"
#include "test.h"

using tcall::call;
namespace reg = rt::shims::console_registry;

namespace {
constexpr uint64_t kHklm = 0xFFFFFFFF80000002ull, kHkcu = 0xFFFFFFFF80000001ull;
struct EnvGuard {
    std::string name;
    explicit EnvGuard(const char* n, const char* v) : name(n) {
        if (v) setenv(n, v, 1);
        else unsetenv(n);
    }
    ~EnvGuard() { unsetenv(name.c_str()); }
};
}  // namespace

TEST(registry_console_mode_answers_from_device_type) {
    EnvGuard g("LEONIDA_DEVICE_TYPE", nullptr);
    uint64_t key = 0;
    const char16_t* oem = u"software\\microsoft\\windows nt\\currentversion\\oem";
    CHECK_EQ(call<uint32_t>("advapi32.dll", "RegOpenKeyW", kHklm, oem, &key), 0u);
    uint32_t type = 0, value = 0, size = 4;
    CHECK_EQ(call<uint32_t>("advapi32.dll", "RegQueryValueExW", key, u"ConsoleMode", static_cast<uint32_t*>(nullptr), &type, reinterpret_cast<uint8_t*>(&value), &size), 0u);
    CHECK(type == 4 && value == 0x64 && size == 4);
    size = 2;
    CHECK_EQ(call<uint32_t>("advapi32.dll", "RegQueryValueExW", key, u"CONSOLEMODE", static_cast<uint32_t*>(nullptr), static_cast<uint32_t*>(nullptr), reinterpret_cast<uint8_t*>(&value), &size), 234u);
    CHECK_EQ(size, 4u);
    size = 0;
    CHECK_EQ(call<uint32_t>("advapi32.dll", "RegQueryValueExW", key, u"ConsoleMode", static_cast<uint32_t*>(nullptr), &type, static_cast<uint8_t*>(nullptr), &size), 0u);
    CHECK_EQ(size, 4u);
    CHECK_EQ(call<uint32_t>("advapi32.dll", "RegQueryValueExW", key, u"Other", static_cast<uint32_t*>(nullptr), &type, static_cast<uint8_t*>(nullptr), &size), 2u);
    CHECK_EQ(call<uint32_t>("advapi32.dll", "RegCloseKey", key), 0u);
    CHECK_EQ(call<uint32_t>("advapi32.dll", "RegCloseKey", key), 6u);

    for (auto [dev, mode] : {std::pair{6, 0x60}, std::pair{8, 0x68}}) {
        setenv("LEONIDA_DEVICE_TYPE", std::to_string(dev).c_str(), 1);
        CHECK_EQ(call<uint32_t>("advapi32.dll", "RegOpenKeyExW", kHklm, oem, 0u, 1u, &key), 0u);
        size = 4;
        CHECK_EQ(call<uint32_t>("advapi32.dll", "RegQueryValueExW", key, u"ConsoleMode", static_cast<uint32_t*>(nullptr), &type, reinterpret_cast<uint8_t*>(&value), &size), 0u);
        CHECK_EQ(value, static_cast<uint32_t>(mode));
        call<uint32_t>("advapi32.dll", "RegCloseKey", key);
    }
    setenv("LEONIDA_DEVICE_TYPE", "1", 1);
    CHECK_EQ(call<uint32_t>("advapi32.dll", "RegOpenKeyW", kHklm, oem, &key), 2u);
}

TEST(registry_directory_backed_store_persists_values) {
    const auto dir = std::filesystem::temp_directory_path() / "gta6rt-reg-test";
    std::filesystem::remove_all(dir);
    reg::set_store_dir(dir.string());
    uint64_t key = 0;
    uint32_t disposition = 0;
    CHECK_EQ(call<uint32_t>("advapi32.dll", "RegCreateKeyExW", kHkcu, u"Software\\Rockstar\\GTA VI", 0u, static_cast<char16_t*>(nullptr), 0u, 0xF003Fu, static_cast<void*>(nullptr), &key, &disposition), 0u);
    CHECK_EQ(disposition, 1u);
    const uint32_t v = 42;
    CHECK_EQ(call<uint32_t>("advapi32.dll", "RegSetValueExW", key, u"Quality", 0u, 4u, reinterpret_cast<const uint8_t*>(&v), 4u), 0u);
    CHECK_EQ(call<uint32_t>("advapi32.dll", "RegSetValueExW", key, u"Name", 0u, 1u, reinterpret_cast<const uint8_t*>(u"abc"), 8u), 0u);
    call<uint32_t>("advapi32.dll", "RegCloseKey", key);

    uint64_t again = 0;
    CHECK_EQ(call<uint32_t>("advapi32.dll", "RegOpenKeyExW", kHkcu, u"software\\rockstar\\gta vi", 0u, 1u, &again), 0u);
    uint32_t type = 0, got = 0, size = 4;
    CHECK_EQ(call<uint32_t>("advapi32.dll", "RegQueryValueExW", again, u"Quality", static_cast<uint32_t*>(nullptr), &type, reinterpret_cast<uint8_t*>(&got), &size), 0u);
    CHECK(type == 4 && got == 42);
    char16_t name[8] = {};
    size = sizeof name;
    CHECK_EQ(call<uint32_t>("advapi32.dll", "RegQueryValueExW", again, u"Name", static_cast<uint32_t*>(nullptr), &type, reinterpret_cast<uint8_t*>(name), &size), 0u);
    CHECK(type == 1 && size == 8 && std::u16string(name) == u"abc");
    CHECK_EQ(call<uint32_t>("advapi32.dll", "RegDeleteValueW", again, u"Quality"), 0u);
    CHECK_EQ(call<uint32_t>("advapi32.dll", "RegQueryValueExW", again, u"Quality", static_cast<uint32_t*>(nullptr), &type, static_cast<uint8_t*>(nullptr), &size), 2u);
    call<uint32_t>("advapi32.dll", "RegCloseKey", again);
    CHECK_EQ(call<uint32_t>("advapi32.dll", "RegOpenKeyExW", kHkcu, u"software\\nothing", 0u, 1u, &again), 2u);
    reg::set_store_dir("");
    std::filesystem::remove_all(dir);
}

TEST(bcrypt_serves_rng_only_and_tracks_handles) {
    void* alg = nullptr;
    CHECK_EQ(call<int32_t>("bcrypt.dll", "BCryptOpenAlgorithmProvider", &alg, u"RNG", static_cast<const char16_t*>(nullptr), 0u), 0);
    CHECK(alg != nullptr);
    uint8_t a[32] = {}, b[32] = {};
    CHECK_EQ(call<int32_t>("bcrypt.dll", "BCryptGenRandom", alg, a, 32u, 0u), 0);
    CHECK_EQ(call<int32_t>("bcrypt.dll", "BCryptGenRandom", static_cast<void*>(nullptr), b, 32u, 2u), 0);
    CHECK(std::memcmp(a, b, 32) != 0);
    CHECK_EQ(call<int32_t>("bcrypt.dll", "BCryptGenRandom", alg, a, 32u, 2u), static_cast<int32_t>(0xC000000Du));
    CHECK_EQ(call<int32_t>("bcrypt.dll", "BCryptGenRandom", reinterpret_cast<void*>(0x1234), a, 32u, 0u), static_cast<int32_t>(0xC0000008u));
    CHECK_EQ(call<int32_t>("bcrypt.dll", "BCryptCloseAlgorithmProvider", alg, 0u), 0);
    CHECK_EQ(call<int32_t>("bcrypt.dll", "BCryptCloseAlgorithmProvider", alg, 0u), static_cast<int32_t>(0xC0000008u));
    void* aes = nullptr;
    CHECK_EQ(call<int32_t>("bcrypt.dll", "BCryptOpenAlgorithmProvider", &aes, u"AES", static_cast<const char16_t*>(nullptr), 0u), static_cast<int32_t>(0xC0000225u));
    CHECK(aes == nullptr);
}

TEST(bcrypt_seed_is_deterministic) {
    rt::bcrypt_rng::reseed_for_test("12345");
    uint8_t a[24], b[24];
    CHECK_EQ(call<int32_t>("bcrypt.dll", "BCryptGenRandom", static_cast<void*>(nullptr), a, 24u, 2u), 0);
    rt::bcrypt_rng::reseed_for_test("12345");
    CHECK_EQ(call<int32_t>("bcrypt.dll", "BCryptGenRandom", static_cast<void*>(nullptr), b, 24u, 2u), 0);
    CHECK(std::memcmp(a, b, 24) == 0);
    rt::bcrypt_rng::reseed_for_test(nullptr);
    CHECK_EQ(call<int32_t>("bcrypt.dll", "BCryptGenRandom", static_cast<void*>(nullptr), b, 24u, 2u), 0);
    CHECK(std::memcmp(a, b, 24) != 0);
}

namespace {
int32_t fake_factory(uint32_t, const rt::com_shim::Guid*, void** ppv) {
    *ppv = reinterpret_cast<void*>(0xABCD);
    return 0;
}
}  // namespace

TEST(com_apartments_nest_and_classes_are_served_by_registration) {
    CHECK_EQ(call<int32_t>("api-ms-win-core-com-l1-1-0.dll", "CoInitializeEx", static_cast<void*>(nullptr), 0u), 0);
    CHECK_EQ(call<int32_t>("api-ms-win-core-com-l1-1-0.dll", "CoInitializeEx", static_cast<void*>(nullptr), 0u), 1);
    CHECK_EQ(call<int32_t>("api-ms-win-core-com-l1-1-0.dll", "CoInitializeEx", static_cast<void*>(nullptr), 2u), static_cast<int32_t>(0x80010106u));
    CHECK_EQ(call<int32_t>("api-ms-win-core-com-l1-1-0.dll", "CoInitializeEx", reinterpret_cast<void*>(1), 0u), static_cast<int32_t>(0x80070057u));
    CHECK_EQ(rt::com_shim::thread_depth(), 2u);
    call<void>("api-ms-win-core-com-l1-1-0.dll", "CoUninitialize");
    call<void>("api-ms-win-core-com-l1-1-0.dll", "CoUninitialize");
    call<void>("api-ms-win-core-com-l1-1-0.dll", "CoUninitialize");
    CHECK_EQ(rt::com_shim::thread_depth(), 0u);

    rt::com_shim::Guid clsid{0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}}, iid{1, 2, 3, {}};
    void* out = reinterpret_cast<void*>(1);
    CHECK_EQ(call<int32_t>("api-ms-win-core-com-l1-1-0.dll", "CoCreateInstance", &clsid, static_cast<void*>(nullptr), 1u, &iid, &out), static_cast<int32_t>(0x80040154u));
    CHECK(out == nullptr);
    rt::com_shim::register_class(clsid, &fake_factory);
    CHECK_EQ(call<int32_t>("api-ms-win-core-com-l1-1-0.dll", "CoCreateInstance", &clsid, static_cast<void*>(nullptr), 1u, &iid, &out), 0);
    CHECK(out == reinterpret_cast<void*>(0xABCD));
    CHECK_EQ(call<int32_t>("api-ms-win-core-com-l1-1-0.dll", "CoCreateInstance", &clsid, reinterpret_cast<void*>(8), 1u, &iid, &out), static_cast<int32_t>(0x80040110u));
}

TEST(com_apartment_type_and_host_entry_points) {
    int32_t type = -9, qual = -9;
    CHECK_EQ(call<int32_t>("ole32.dll", "CoGetApartmentType", &type, &qual), static_cast<int32_t>(0x800401F0u));
    CHECK_EQ(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), 0);
    CHECK_EQ(call<int32_t>("ole32.dll", "CoGetApartmentType", &type, &qual), 0);
    CHECK_EQ(type, 0);
    ::CoUninitialize();
    CHECK_EQ(::CoInitializeEx(nullptr, COINIT_MULTITHREADED), 0);
    CHECK_EQ(call<int32_t>("ole32.dll", "CoGetApartmentType", &type, &qual), 0);
    CHECK_EQ(type, 1);
    ::CoUninitialize();
    void* p = ::CoTaskMemAlloc(16);
    CHECK(p != nullptr);
    ::CoTaskMemFree(p);
    GUID g{};
    CHECK_EQ(::CoCreateGuid(&g), 0);
    char16_t buf[40] = {};
    CHECK_EQ(::StringFromGUID2(g, buf, 40), 39);
}

TEST(com_guid_string_and_propvariant) {
    rt::com_shim::Guid g{};
    CHECK_EQ(call<int32_t>("api-ms-win-core-com-l1-1-0.dll", "CoCreateGuid", &g), 0);
    CHECK_EQ(g.data3 >> 12, 4);
    CHECK_EQ(g.data4[0] >> 6, 2);
    rt::com_shim::Guid fixed{0x12345678, 0x9ABC, 0xDEF0, {1, 2, 3, 4, 5, 6, 7, 8}};
    char16_t text[40];
    CHECK_EQ(call<int>("api-ms-win-core-com-l1-1-0.dll", "StringFromGUID2", &fixed, text, 40), 39);
    CHECK(std::u16string(text) == u"{12345678-9ABC-DEF0-0102-030405060708}");
    CHECK_EQ(call<int>("api-ms-win-core-com-l1-1-0.dll", "StringFromGUID2", &fixed, text, 38), 0);
    uint8_t pv[24] = {};
    const uint16_t vt = 31;
    std::memcpy(pv, &vt, 2);
    void* s = std::malloc(16);
    std::memcpy(pv + 8, &s, 8);
    CHECK_EQ(call<int32_t>("api-ms-win-core-com-l1-1-0.dll", "PropVariantClear", pv), 0);
    CHECK_EQ(pv[0] | pv[8], 0);
    void* m = call<void*>("api-ms-win-core-com-l1-1-0.dll", "CoTaskMemAlloc", size_t{8});
    CHECK(m != nullptr);
    call<void>("api-ms-win-core-com-l1-1-0.dll", "CoTaskMemFree", m);
}

namespace {
std::atomic<int> g_status_callbacks{0};
std::atomic<uint32_t> g_last_status{0};
std::atomic<uint64_t> g_last_context{0};
uint64_t WINAPI winhttp_callback(uint64_t handle, uint64_t context, uint64_t status, uint64_t info, uint64_t length) {
    ++g_status_callbacks;
    g_last_status = static_cast<uint32_t>(status);
    g_last_context = context;
    (void)handle; (void)info; (void)length;
    return 0;
}
constexpr const char* kH = "winhttp.dll";
}  // namespace

TEST(winhttp_offline_sync_request_fails_to_connect) {
    uint64_t session = call<uint64_t>(kH, "WinHttpOpen", u"agent", 0u, static_cast<const char16_t*>(nullptr), static_cast<const char16_t*>(nullptr), 0u);
    CHECK(session != 0);
    uint64_t connection = call<uint64_t>(kH, "WinHttpConnect", session, u"example.com", uint16_t{443}, 0u);
    CHECK(connection != 0);
    CHECK_EQ(call<uint64_t>(kH, "WinHttpConnect", session, u"", uint16_t{443}, 0u), 0u);
    uint64_t request = call<uint64_t>(kH, "WinHttpOpenRequest", connection, u"GET", u"/", static_cast<const char16_t*>(nullptr), static_cast<const char16_t*>(nullptr), static_cast<const char16_t**>(nullptr), 0u);
    CHECK(request != 0);
    CHECK_EQ(call<int>(kH, "WinHttpSendRequest", request, static_cast<const char16_t*>(nullptr), 0u, static_cast<void*>(nullptr), 0u, 0u, uint64_t{0}), 0);
    CHECK_EQ(call<uint32_t>("kernel32.dll", "GetLastError"), 12029u);
    CHECK_EQ(call<int>(kH, "WinHttpReceiveResponse", request, static_cast<void*>(nullptr)), 0);
    CHECK_EQ(call<uint32_t>("kernel32.dll", "GetLastError"), 12019u);
    CHECK_EQ(call<int>(kH, "WinHttpAddRequestHeaders", request, u"Accept: */*\r\n", 0xFFFFFFFFu, 0u), 1);
    CHECK_EQ(call<int>(kH, "WinHttpAddRequestHeaders", request, u"bad header\r\n", 0xFFFFFFFFu, 0u), 0);
    CHECK_EQ(call<int>(kH, "WinHttpSetTimeouts", request, 0, 1000, 1000, -1), 1);
    CHECK_EQ(call<int>(kH, "WinHttpSetTimeouts", request, 0, -5, 1000, -1), 0);
    uint64_t ctx = 0x77, got = 0;
    uint32_t len = 8;
    CHECK_EQ(call<int>(kH, "WinHttpSetOption", request, 45u, &ctx, 8u), 1);
    CHECK_EQ(call<int>(kH, "WinHttpQueryOption", request, 45u, &got, &len), 1);
    CHECK_EQ(got, 0x77u);
    uint32_t type = 0;
    len = 4;
    CHECK_EQ(call<int>(kH, "WinHttpQueryOption", request, 9u, &type, &len), 1);
    CHECK_EQ(type, 3u);
    CHECK_EQ(call<int>(kH, "WinHttpQueryOption", request, 999u, &type, &len), 0);
    CHECK_EQ(call<int>(kH, "WinHttpCloseHandle", request), 1);
    CHECK_EQ(call<int>(kH, "WinHttpCloseHandle", request), 0);
    CHECK_EQ(call<int>(kH, "WinHttpCloseHandle", connection), 1);
    CHECK_EQ(call<int>(kH, "WinHttpCloseHandle", session), 1);
}

TEST(winhttp_async_failure_arrives_through_the_status_callback) {
    g_status_callbacks = 0;
    uint64_t session = call<uint64_t>(kH, "WinHttpOpen", u"agent", 0u, static_cast<const char16_t*>(nullptr), static_cast<const char16_t*>(nullptr), 0x10000000u);
    call<uint64_t>(kH, "WinHttpSetStatusCallback", session, reinterpret_cast<uint64_t>(&winhttp_callback), 0xFFFFFFFFu, uint64_t{0});
    uint64_t connection = call<uint64_t>(kH, "WinHttpConnect", session, u"example.com", uint16_t{443}, 0u);
    uint64_t request = call<uint64_t>(kH, "WinHttpOpenRequest", connection, u"GET", u"/", static_cast<const char16_t*>(nullptr), static_cast<const char16_t*>(nullptr), static_cast<const char16_t**>(nullptr), 0u);
    CHECK(request != 0);
    CHECK_EQ(g_status_callbacks.load(), 2);
    CHECK_EQ(call<int>(kH, "WinHttpSendRequest", request, static_cast<const char16_t*>(nullptr), 0u, static_cast<void*>(nullptr), 0u, 0u, uint64_t{0x99}), 1);
    for (int i = 0; i < 200 && g_status_callbacks.load() < 3; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_EQ(g_status_callbacks.load(), 3);
    CHECK_EQ(g_last_status.load(), 0x00200000u);
    CHECK_EQ(g_last_context.load(), 0x99u);
    call<int>(kH, "WinHttpCloseHandle", request);
    call<int>(kH, "WinHttpCloseHandle", connection);
    call<int>(kH, "WinHttpCloseHandle", session);
    CHECK(g_status_callbacks.load() >= 6);
}

TEST(winhttp_crack_url) {
    struct Url {
        uint32_t size;
        char16_t* scheme;
        uint32_t scheme_len, scheme_id;
        char16_t* host;
        uint32_t host_len;
        uint16_t port;
        char16_t* user;
        uint32_t user_len;
        char16_t* pass;
        uint32_t pass_len;
        char16_t* path;
        uint32_t path_len;
        char16_t* extra;
        uint32_t extra_len;
    } u{};
    u.size = sizeof u;
    u.scheme_len = u.host_len = u.user_len = u.pass_len = u.path_len = u.extra_len = static_cast<uint32_t>(-1);
    const char16_t* url = u"https://user:pw@example.com:8443/a/b?x=1#f";
    CHECK_EQ(call<int>(kH, "WinHttpCrackUrl", url, 0u, 0u, &u), 1);
    CHECK(u.scheme_id == 2 && u.port == 8443);
    CHECK(std::u16string(u.host, u.host_len) == u"example.com");
    CHECK(std::u16string(u.path, u.path_len) == u"/a/b");
    CHECK(std::u16string(u.extra, u.extra_len) == u"?x=1#f");
    CHECK(std::u16string(u.user, u.user_len) == u"user" && std::u16string(u.pass, u.pass_len) == u"pw");
    u = {};
    u.size = sizeof u;
    char16_t hostbuf[4];
    u.host = hostbuf;
    u.host_len = 4;
    CHECK_EQ(call<int>(kH, "WinHttpCrackUrl", u"http://example.com/", 0u, 0u, &u), 0);
    CHECK_EQ(call<uint32_t>("kernel32.dll", "GetLastError"), 122u);
    CHECK_EQ(u.host_len, 12u);
    u = {};
    u.size = sizeof u;
    CHECK_EQ(call<int>(kH, "WinHttpCrackUrl", u"ftp://x/", 0u, 0u, &u), 0);
    CHECK_EQ(call<uint32_t>("kernel32.dll", "GetLastError"), 12006u);
    CHECK_EQ(call<int>(kH, "WinHttpCrackUrl", u"nonsense", 0u, 0u, &u), 0);
}

TEST(lifecycle_callbacks_and_window_fit) {
    CHECK_EQ(rt::lifecycle::registered_notification_count(), 0u);
    uint64_t reg_id = 0;
    CHECK_EQ(call<uint32_t>("api-ms-win-core-psm-appnotify-l1-1-0.dll", "RegisterAppStateChangeNotification", uint64_t{0x1000}, uint64_t{5}, &reg_id), 0u);
    CHECK(reg_id != 0);
    CHECK_EQ(call<uint32_t>("api-ms-win-core-psm-appnotify-l1-1-0.dll", "RegisterAppStateChangeNotification", uint64_t{0}, uint64_t{5}, &reg_id), 87u);
    CHECK_EQ(rt::lifecycle::registered_notification_count(), 1u);
    call<void>("api-ms-win-core-psm-appnotify-l1-1-0.dll", "UnregisterAppStateChangeNotification", reg_id);
    CHECK_EQ(rt::lifecycle::registered_notification_count(), 0u);

    const rt::lifecycle::Rect area{0, 0, 1920, 1080};
    CHECK(rt::lifecycle::fit_window_to_area(area, 3840, 2160) == std::make_pair(1920, 1080));
    CHECK(rt::lifecycle::fit_window_to_area(area, 1280, 720) == std::make_pair(1280, 720));
    CHECK(rt::lifecycle::fit_window_to_area(area, 3840, 1080) == std::make_pair(1920, 540));
    CHECK(rt::lifecycle::fit_window_to_area(area, -1, 100) == std::make_pair(-1, 100));
}

namespace {
std::atomic<int> g_suspend_calls{0};
std::atomic<uint64_t> g_suspend_quiesced{0}, g_suspend_context{0};
uint64_t WINAPI suspend_routine(uint64_t quiesced, uint64_t context) {
    ++g_suspend_calls;
    g_suspend_quiesced = quiesced;
    g_suspend_context = context;
    return 0;
}
}  // namespace

TEST(lifecycle_suspend_runs_registered_routines_with_quiesced_true) {
    uint64_t id = 0;
    call<uint32_t>("api-ms-win-core-psm-appnotify-l1-1-0.dll", "RegisterAppStateChangeNotification", reinterpret_cast<uint64_t>(&suspend_routine), uint64_t{0x55}, &id);
    rt::lifecycle::invoke_suspend_callbacks_for_test();
    CHECK_EQ(g_suspend_calls.load(), 1);
    CHECK_EQ(g_suspend_quiesced.load(), 1u);
    CHECK_EQ(g_suspend_context.load(), 0x55u);
    call<void>("api-ms-win-core-psm-appnotify-l1-1-0.dll", "UnregisterAppStateChangeNotification", id);
}
