#include "test.h"

#include <cstring>

#include <gta6rt/os.h>

#include "env_stub.h"
#include "shims.h"

namespace {

int WINAPI add_three(int a, int b, int c) { return a + b + c; }
int WINAPI add_eight(int a, int b, int c, int d, int e, int f, int g, int h) { return a + b + c + d + e + f + g + h; }
double WINAPI scale(double x, int n) { return x * n; }
void* WINAPI echo_ptr(void* p) { return p; }

int g_data = 7;

SHIM_TABLE(test_registry,
    SHIM("test.dll", "AddThree", add_three),
    SHIM("test.dll", "AddEight", add_eight),
    SHIM("test.dll", "Scale", scale),
    SHIM("TEST.DLL", "EchoPtr", echo_ptr),
    SHIM_DATA("test.dll", "Data", &g_data),
    SHIM("test.dll", "#42", add_three),
);

gta6rt_import_ref ref(const char* dll, const char* name) {
    gta6rt_import_ref r{};
    r.dll = dll;
    r.name = name;
    r.kind_guess = GTA6RT_IMPORT_CODE;
    return r;
}

}  // namespace

TEST(registry_find_case_insensitive) {
    CHECK(rt::shims::find("test.dll", "AddThree") != nullptr);
    CHECK(rt::shims::find("test.dll", "EchoPtr") != nullptr);
    CHECK(rt::shims::find("test.dll", "Nope") == nullptr);
    CHECK(rt::shims::owns_dll("test.dll"));
    CHECK(!rt::shims::owns_dll("absent.dll"));
    CHECK_EQ(rt::shims::count_rows("test.dll"), 6u);
}

TEST(registry_resolver_binds_code) {
    gta6rt_import_ref r = ref("TEST.dll", "AddThree");
    gta6rt_import_binding b{};
    CHECK_EQ(gta6rt_shims_import_resolver(nullptr, &r, &b), GTA6RT_OK);
    const stub::ThunkRecord* th = stub::thunk(b.address);
    CHECK(th != nullptr);
    if (!th) return;
#if defined(__x86_64__)
    CHECK(th->kind == GTA6RT_THUNK_NATIVE);
    auto fn = reinterpret_cast<int(WINAPI*)(int, int, int)>(const_cast<void*>(th->fn));
    CHECK_EQ(fn(1, 2, 3), 6);
#else
    CHECK(th->kind == GTA6RT_THUNK_FRAME);
    gta6rt_guest_frame f{};
    f.rcx = 1;
    f.rdx = 2;
    f.r8 = 3;
    reinterpret_cast<gta6rt_frame_handler>(const_cast<void*>(th->fn))(&f, th->user);
    CHECK_EQ(static_cast<int>(f.rax), 6);
#endif
    gta6rt_import_binding b2{};
    CHECK_EQ(gta6rt_shims_import_resolver(nullptr, &r, &b2), GTA6RT_OK);
    CHECK_EQ(b2.address, b.address);
}

TEST(registry_resolver_ordinal_and_data) {
    gta6rt_import_ref r{};
    r.dll = "test.dll";
    r.name = nullptr;
    r.ordinal = 42;
    gta6rt_import_binding b{};
    CHECK_EQ(gta6rt_shims_import_resolver(nullptr, &r, &b), GTA6RT_OK);
    CHECK(stub::thunk(b.address) != nullptr);

    gta6rt_import_ref d = ref("test.dll", "Data");
    d.kind_guess = GTA6RT_IMPORT_DATA;
    CHECK_EQ(gta6rt_shims_import_resolver(nullptr, &d, &b), GTA6RT_OK);
    CHECK_EQ(b.address, reinterpret_cast<uintptr_t>(&g_data));
}

TEST(registry_unserved_import_is_named_stub) {
    gta6rt_import_ref r = ref("test.dll", "DoesNotExist");
    gta6rt_import_binding b{};
    CHECK_EQ(gta6rt_shims_import_resolver(nullptr, &r, &b), GTA6RT_OK);
    const stub::ThunkRecord* th = stub::thunk(b.address);
    CHECK(th != nullptr);
    if (!th) return;
    CHECK(th->kind == GTA6RT_THUNK_TRAP);
    CHECK(std::strstr(th->name, "test.dll!DoesNotExist") != nullptr);
    rt::shims::set_unimplemented_policy(rt::shims::UnimplementedPolicy::Log);
    const size_t before = rt::shims::unimplemented_call_count();
    gta6rt_guest_frame f{};
    f.rax = 0xdead;
    uint64_t ret_slot = 0x1234;
    f.rsp = reinterpret_cast<uintptr_t>(&ret_slot);
    reinterpret_cast<gta6rt_frame_handler>(const_cast<void*>(th->fn))(&f, th->user);
    CHECK_EQ(f.rax, 0u);
    CHECK_EQ(rt::shims::unimplemented_call_count(), before + 1);
}

TEST(registry_policy_fail_returns_enotimpl_for_hresult_names) {
    gta6rt_import_ref r = ref("xgameruntime.dll", "XUserAddAsync");
    gta6rt_import_binding b{};
    CHECK_EQ(gta6rt_shims_import_resolver(nullptr, &r, &b), GTA6RT_OK);
    const stub::ThunkRecord* th = stub::thunk(b.address);
    CHECK(th != nullptr);
    if (!th) return;
    rt::shims::set_unimplemented_policy(rt::shims::UnimplementedPolicy::Fail);
    gta6rt_guest_frame f{};
    uint64_t ret_slot = 0;
    f.rsp = reinterpret_cast<uintptr_t>(&ret_slot);
    reinterpret_cast<gta6rt_frame_handler>(const_cast<void*>(th->fn))(&f, th->user);
    CHECK_EQ(static_cast<uint32_t>(f.rax), 0x80004001u);
    rt::shims::set_unimplemented_policy(rt::shims::UnimplementedPolicy::Log);
}

TEST(registry_policy_parse) {
    bool valid = true;
    CHECK(rt::shims::parse_unimplemented_policy("STOP", &valid) == rt::shims::UnimplementedPolicy::Stop && valid);
    CHECK(rt::shims::parse_unimplemented_policy("fail", &valid) == rt::shims::UnimplementedPolicy::Fail && valid);
    CHECK(rt::shims::parse_unimplemented_policy("log", &valid) == rt::shims::UnimplementedPolicy::Log && valid);
    CHECK(rt::shims::parse_unimplemented_policy("", &valid) == rt::shims::UnimplementedPolicy::Fail && valid);
    CHECK(rt::shims::parse_unimplemented_policy(nullptr, &valid) == rt::shims::UnimplementedPolicy::Fail && valid);
    CHECK(rt::shims::parse_unimplemented_policy("bogus", &valid) == rt::shims::UnimplementedPolicy::Fail && !valid);
}

TEST(registry_policy_fail_is_null_with_last_error_for_other_names) {
    gta6rt_import_ref r = ref("crypt32.dll", "CertOpenSystemStoreW");
    gta6rt_import_binding b{};
    CHECK_EQ(gta6rt_shims_import_resolver(nullptr, &r, &b), GTA6RT_OK);
    const stub::ThunkRecord* th = stub::thunk(b.address);
    CHECK(th != nullptr);
    if (!th) return;
    rt::shims::set_unimplemented_policy(rt::shims::UnimplementedPolicy::Fail);
    const size_t before = rt::shims::unimplemented_call_count();
    gta6rt_last_error_set(0);
    for (int i = 0; i < 3; ++i) {
        gta6rt_guest_frame f{};
        f.rax = 0xdead;
        uint64_t ret_slot = 0;
        f.rsp = reinterpret_cast<uintptr_t>(&ret_slot);
        reinterpret_cast<gta6rt_frame_handler>(const_cast<void*>(th->fn))(&f, th->user);
        CHECK_EQ(f.rax, 0u);
    }
    CHECK_EQ(gta6rt_last_error_get(), 120u);
    CHECK_EQ(rt::shims::unimplemented_call_count(), before + 3);
    rt::shims::set_unimplemented_policy(rt::shims::UnimplementedPolicy::Log);
}

TEST(registry_return_classes_of_the_titles_unserved_imports) {
    using rt::shims::ReturnClass;
    CHECK(rt::shims::classify_import("mfplat.dll", "MFStartup") == ReturnClass::HResult);
    CHECK(rt::shims::classify_import("mfplat.dll", "MFCreateAttributes") == ReturnClass::HResult);
    CHECK(rt::shims::classify_import("mfreadwrite.dll", "MFCreateSourceReaderFromMediaSource") == ReturnClass::HResult);
    CHECK(rt::shims::classify_import("mfplat.dll", "MFGetSystemTime") == ReturnClass::Other);
    CHECK(rt::shims::classify_import("xaudio2_9.dll", "#1") == ReturnClass::HResult);
    CHECK(rt::shims::classify_import("crypt32.dll", "CertCloseStore") == ReturnClass::Other);
    CHECK(rt::shims::classify_import("api-ms-win-crt-environment-l1-1-0.dll", "_wputenv_s") == ReturnClass::Errno);
    CHECK(rt::shims::classify_import("api-ms-win-crt-stdio-l1-1-0.dll", "__stdio_common_vsprintf_s") == ReturnClass::Other);
    CHECK(rt::shims::classify_import("api-ms-win-crt-string-l1-1-0.dll", "strnlen_s") == ReturnClass::Other);

    gta6rt_import_ref r = ref("api-ms-win-crt-environment-l1-1-0.dll", "_wputenv_s");
    gta6rt_import_binding b{};
    CHECK_EQ(gta6rt_shims_import_resolver(nullptr, &r, &b), GTA6RT_OK);
    const stub::ThunkRecord* th = stub::thunk(b.address);
    CHECK(th != nullptr);
    if (!th) return;
    rt::shims::set_unimplemented_policy(rt::shims::UnimplementedPolicy::Fail);
    gta6rt_guest_frame f{};
    uint64_t ret_slot = 0;
    f.rsp = reinterpret_cast<uintptr_t>(&ret_slot);
    reinterpret_cast<gta6rt_frame_handler>(const_cast<void*>(th->fn))(&f, th->user);
    CHECK_EQ(f.rax, 22u);
    rt::shims::set_unimplemented_policy(rt::shims::UnimplementedPolicy::Log);
}

TEST(registry_putenv_s_sets_and_clears) {
    using Fn = int(WINAPI*)(const char*, const char*);
    auto fn = reinterpret_cast<Fn>(rt::shims::find("api-ms-win-crt-environment-l1-1-0.dll", "_putenv_s"));
    CHECK(fn != nullptr);
    if (!fn) return;
    CHECK_EQ(fn("GTA6RT_PUTENV_PROBE", "1"), 0);
    const char* v = gta6rt_env_get("GTA6RT_PUTENV_PROBE");
    CHECK(v && std::strcmp(v, "1") == 0);
    CHECK_EQ(fn("GTA6RT_PUTENV_PROBE", ""), 0);
    CHECK(gta6rt_env_get("GTA6RT_PUTENV_PROBE") == nullptr);
    CHECK_EQ(fn("A=B", "1"), 22);
    CHECK_EQ(fn(nullptr, "1"), 22);
}

TEST(registry_api_set_alias_falls_back_to_family) {
    static int dummy_k32 = 0;
    gta6rt_shim_export exports[] = {
        {"AliasProbe", 0, GTA6RT_SHIM_DATA, &dummy_k32},
        {nullptr, 0, GTA6RT_SHIM_DATA, nullptr},
    };
    CHECK_EQ(gta6rt_shims_register_dll("kernel32.dll", exports), GTA6RT_OK);
    CHECK(rt::shims::find("api-ms-win-core-synch-l1-2-0.dll", "AliasProbe") != nullptr);
    CHECK(rt::shims::find("kernelbase.dll", "AliasProbe") != nullptr);
    CHECK(rt::shims::find("api-ms-win-crt-string-l1-1-0.dll", "AliasProbe") == nullptr);
    CHECK(rt::shims::owns_dll("api-ms-win-core-foo-l1-1-0.dll"));
}

TEST(registry_frame_adapter_args) {
#if !defined(__x86_64__)
    uint64_t stack[16] = {};
    gta6rt_guest_frame f{};
    stack[0] = 0;
    f.rsp = reinterpret_cast<uintptr_t>(stack);
    f.rcx = 1; f.rdx = 2; f.r8 = 3; f.r9 = 4;
    stack[5] = 5; stack[6] = 6; stack[7] = 7; stack[8] = 8;
    rt::shims::detail::frame_handler<&add_eight>(&f, nullptr);
    CHECK_EQ(static_cast<int>(f.rax), 36);

    gta6rt_guest_frame g{};
    g.xmm0 = 0;
    double x = 2.5;
    std::memcpy(&g.xmm0, &x, 8);
    g.rdx = 4;
    rt::shims::detail::frame_handler<&scale>(&g, nullptr);
    double r;
    std::memcpy(&r, &g.xmm0_ret, 8);
    CHECK(r == 10.0 && g.ret_is_float == 1);
#endif
}
