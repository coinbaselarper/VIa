#include <windows.h>

#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <gta6rt/isa.h>
#include <gta6rt/os.h>
#include <gta6rt/shims.h>

namespace {
int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(c)) {                                                       \
            ++g_fail;                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
        }                                                                 \
    } while (0)

gta6rt_gva bind(const char* dll, const char* name, bool* trap = nullptr) {
    gta6rt_import_ref r{};
    r.dll = dll;
    r.name = name;
    r.kind_guess = GTA6RT_IMPORT_CODE;
    gta6rt_import_binding b{};
    if (gta6rt_shims_import_resolver(nullptr, &r, &b) != GTA6RT_OK) return 0;
    (void)trap;
    return b.address;
}

uint64_t call(gta6rt_gva fn, std::initializer_list<uint64_t> args, uint32_t float_mask = 0) {
    gta6rt_guest_args a{};
    for (uint64_t v : args) a.args[a.count++] = v;
    a.float_mask = float_mask;
    gta6rt_guest_ret r{};
    if (gta6rt_isa_call_guest(fn, &a, &r) != GTA6RT_OK) {
        ++g_fail;
        std::fprintf(stderr, "FAIL call_guest\n");
        return ~0ull;
    }
    return r.rax;
}

uint64_t u(const void* p) { return reinterpret_cast<uintptr_t>(p); }

int g_completion_calls = 0;
uint32_t g_completion_bytes = 0, g_completion_error = 99;
void WINAPI completion(DWORD error, DWORD bytes, LPOVERLAPPED) {
    ++g_completion_calls;
    g_completion_error = error;
    g_completion_bytes = bytes;
}
}  // namespace

int main() {
    if (std::strcmp(gta6rt_isa_backend_name(), "rosetta") != 0) {
        std::printf("shims_core_real_smoke: SKIP (backend %s: guest thunks need the rosetta backend)\n", gta6rt_isa_backend_name());
        return 0;
    }
    gta6rt_isa_config cfg{};
    cfg.teb_mode = GTA6RT_TEB_SHARE;
    CHECK(gta6rt_isa_init(&cfg) == GTA6RT_OK);
    static uint8_t peb[0x400];
    gta6rt_isa_set_peb(peb);
    gta6rt_teb_desc d{};
    d.peb = peb;
    d.process_id = static_cast<uint32_t>(getpid());
    d.thread_id = 1;
    d.stack_base = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(pthread_self()));
    d.stack_limit = d.stack_base - pthread_get_stacksize_np(pthread_self());
    CHECK(gta6rt_isa_thread_attach(&d, nullptr) == GTA6RT_OK);
    CHECK(gta6rt_shims_init() == GTA6RT_OK);

    const char* tmp = "/tmp/gta6rt_shims_core_real_smoke";
    std::string game = std::string(tmp) + "/game", overlay = std::string(tmp) + "/overlay";
    CHECK(gta6rt_shims_configure_paths(game.c_str(), overlay.c_str()) == GTA6RT_OK);

    const gta6rt_gva tick = bind("KERNEL32.dll", "GetTickCount64");
    CHECK(tick != 0);
    CHECK(call(tick, {}) != 0);

    const gta6rt_gva pid = bind("api-ms-win-core-processthreads-l1-1-0.dll", "GetCurrentProcessId");
    CHECK(pid != 0);
    CHECK(static_cast<uint32_t>(call(pid, {})) == static_cast<uint32_t>(getpid()));

    const gta6rt_gva wcslen_ = bind("api-ms-win-crt-string-l1-1-0.dll", "wcslen");
    static const char16_t wide[] = u"gta\u00e9\U0001F600";
    CHECK(wcslen_ != 0);
    CHECK(call(wcslen_, {u(wide)}) == 6);

    const gta6rt_gva pow_ = bind("api-ms-win-crt-math-l1-1-0.dll", "pow");
    CHECK(pow_ != 0);
    gta6rt_guest_args fa{};
    fa.count = 2;
    fa.float_mask = 3;
    const double base = 3.0, exponent = 4.0;
    std::memcpy(&fa.args[0], &base, 8);
    std::memcpy(&fa.args[1], &exponent, 8);
    gta6rt_guest_ret fr{};
    CHECK(gta6rt_isa_call_guest(pow_, &fa, &fr) == GTA6RT_OK);
    double res;
    std::memcpy(&res, &fr.xmm0_lo, 8);
    CHECK(res == 81.0);

    const gta6rt_gva create = bind("KERNEL32.dll", "CreateFileW");
    const gta6rt_gva write = bind("KERNEL32.dll", "WriteFile");
    const gta6rt_gva read = bind("KERNEL32.dll", "ReadFile");
    const gta6rt_gva size = bind("KERNEL32.dll", "GetFileSizeEx");
    const gta6rt_gva close = bind("KERNEL32.dll", "CloseHandle");
    CHECK(create && write && read && size && close);
    static const char16_t path[] = u"T:\\smoke.bin";
    const uint64_t h = call(create, {u(path), 0xC0000000u, 0, 0, 2 /*CREATE_ALWAYS*/, 0x80, 0});
    CHECK(h != ~0ull && h != 0);
    const char payload[] = "real os file io";
    uint32_t done = 0;
    CHECK(static_cast<uint32_t>(call(write, {h, u(payload), sizeof payload, u(&done), 0})) == 1);
    CHECK(done == sizeof payload);
    int64_t fsz = 0;
    CHECK(static_cast<uint32_t>(call(size, {h, u(&fsz)})) == 1);
    CHECK(fsz == static_cast<int64_t>(sizeof payload));
    CHECK(static_cast<uint32_t>(call(close, {h})) == 1);
    const uint64_t h2 = call(create, {u(path), 0x80000000u, 1, 0, 3 /*OPEN_EXISTING*/, 0x80, 0});
    CHECK(h2 != ~0ull && h2 != 0);
    char back[64] = {};
    done = 0;
    CHECK(static_cast<uint32_t>(call(read, {h2, u(back), sizeof back, u(&done), 0})) == 1);
    CHECK(done == sizeof payload && std::strcmp(back, payload) == 0);
    call(close, {h2});

    {
        const std::string host = std::string(tmp) + "/os16.bin";
        mkdir(tmp, 0755);
        std::ofstream(host, std::ios::binary) << "0123456789abcdef";
        const std::u16string wpath = u"Z:" + [&] {
            std::u16string w;
            for (char c : host) w.push_back(c == '/' ? u'\\' : static_cast<char16_t>(c));
            return w;
        }();
        const std::u16string hpath(host.begin(), host.end());
        const gta6rt_gva getsize = bind("KERNEL32.dll", "GetFileSizeEx");
        const gta6rt_gva seek = bind("KERNEL32.dll", "SetFilePointerEx");
        const gta6rt_gva create2 = bind("KERNEL32.dll", "CreateFile2");
        const gta6rt_gva getstd = bind("KERNEL32.dll", "GetStdHandle");
        const gta6rt_gva readex = bind("KERNEL32.dll", "ReadFileEx");
        CHECK(getsize && seek && create2 && getstd && readex);

        HANDLE os_h = ::CreateFile2(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
        CHECK(os_h != INVALID_HANDLE_VALUE);
        int64_t osz = 0;
        CHECK(static_cast<uint32_t>(call(getsize, {u(os_h), u(&osz)})) == 1);
        CHECK(osz == 16);
        int64_t npos = 0;
        CHECK(static_cast<uint32_t>(call(seek, {u(os_h), 4, u(&npos), 0})) == 1);
        CHECK(npos == 4);
        char part[32] = {};
        done = 0;
        CHECK(static_cast<uint32_t>(call(read, {u(os_h), u(part), sizeof part, u(&done), 0})) == 1);
        CHECK(done == 12 && std::memcmp(part, "456789abcdef", 12) == 0);
        CHECK(static_cast<uint32_t>(call(close, {u(os_h)})) == 1);

        const uint64_t c2 = call(create2, {u(hpath.c_str()), 0x80000000u, 1, 3, 0});
        CHECK(c2 != ~0ull && c2 != 0);
        osz = 0;
        CHECK(static_cast<uint32_t>(call(getsize, {c2, u(&osz)})) == 1);
        CHECK(osz == 16);
        call(close, {c2});

        const uint64_t out = call(getstd, {static_cast<uint64_t>(static_cast<uint32_t>(-11))});
        CHECK(out != ~0ull && out != 0);
        static const char line[] = "shims_core_real_smoke: core WriteFile on an os GetStdHandle handle\n";
        done = 0;
        CHECK(static_cast<uint32_t>(call(write, {out, u(line), sizeof line - 1, u(&done), 0})) == 1);
        CHECK(done == sizeof line - 1);

        const uint64_t ov_h = call(create, {u(hpath.c_str()), 0x80000000u, 1, 0, 3, 0x40000000u, 0});
        CHECK(ov_h != ~0ull && ov_h != 0);
        OVERLAPPED ov{};
        ov.Offset = 2;
        char exbuf[8] = {};
        CHECK(static_cast<uint32_t>(call(readex, {ov_h, u(exbuf), 6, u(&ov), u(reinterpret_cast<void*>(&completion))})) == 1);
        CHECK(std::memcmp(exbuf, "234567", 6) == 0);
        CHECK(g_completion_calls == 0);
        CHECK(::SleepEx(0, TRUE) == WAIT_IO_COMPLETION);
        CHECK(g_completion_calls == 1 && g_completion_bytes == 6 && g_completion_error == 0);
        call(close, {ov_h});
    }

    const gta6rt_gva nope = bind("KERNEL32.dll", "ThisImportIsNotServed");
    CHECK(nope != 0);
    const size_t before = gta6rt_shims_report_unimplemented();
    call(nope, {});
    CHECK(gta6rt_shims_report_unimplemented() == before + 1);

    std::printf("shims_core_real_smoke: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
