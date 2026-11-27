// isa_vshift_check_test (x86_64/Rosetta): the GTA6RT_ISA_VSHIFT_CHECK stub on generated vpsrlvq/vpsllvq sites
// (register count, [rsp] count, xmm form): x86 results for every count, execution and >= 2^32 counters, the last bad
// count, and the cost per execution against the raw instruction.

#include "x86_patch.h"

#include <gta6rt/isa.h>

#include <mach/mach_time.h>
#include <sys/mman.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

using namespace gta6rt::isa;

namespace {

int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(c)) {                                                           \
            ++g_fail;                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
        }                                                                     \
    } while (0)

using Fn = void(__attribute__((ms_abi)) *)(const uint64_t* src, const uint64_t* cnt, uint64_t* out);

uint8_t* g_code;
size_t g_used;

using Bytes = std::vector<uint8_t>;

uint8_t* emit(const std::vector<Bytes>& parts, size_t* site_off) {
    uint8_t* at = g_code + g_used;
    size_t n = 0, k = 0;
    for (const Bytes& p : parts) {
        if (k++ == 2) *site_off = n;
        for (uint8_t b : p) at[n++] = b;
    }
    g_used = (g_used + n + 63) & ~size_t{63};
    return at;
}

struct Case {
    const char* name;
    bool left, ymm;
    Fn fn;
    uint8_t* site;
    uint8_t raw[8];
    size_t raw_len;
};

uint64_t x86_shift(uint64_t v, uint64_t c, bool left) { return c >= 64 ? 0 : left ? v << c : v >> c; }

uint64_t now_ns() {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}

uint64_t rnd() { return (uint64_t(random()) << 33) ^ (uint64_t(random()) << 11) ^ uint64_t(random()); }

}  // namespace

int main() {
    gta6rt_isa_config cfg{};
    CHECK(gta6rt_isa_init(&cfg) == GTA6RT_OK);
    void* p = mmap(nullptr, 1 << 16, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(p != MAP_FAILED);
    g_code = static_cast<uint8_t*>(p);
    const Bytes tail_y = {0xC4, 0xC1, 0x7E, 0x7F, 0x00, 0xC5, 0xF8, 0x77, 0xC3};
    const Bytes tail_x = {0xC4, 0xC1, 0x7A, 0x7F, 0x00, 0xC5, 0xF8, 0x77, 0xC3};
    Case cases[4] = {
        {"vpsrlvq ymm0, ymm1, ymm2", false, true, nullptr, nullptr, {0xC4, 0xE2, 0xF5, 0x45, 0xC2}, 5},
        {"vpsllvq ymm0, ymm1, ymm2", true, true, nullptr, nullptr, {0xC4, 0xE2, 0xF5, 0x47, 0xC2}, 5},
        {"vpsrlvq ymm0, ymm1, [rsp]", false, true, nullptr, nullptr, {0xC4, 0xE2, 0xF5, 0x45, 0x04, 0x24}, 6},
        {"vpsllvq xmm0, xmm1, xmm2", true, false, nullptr, nullptr, {0xC4, 0xE2, 0xF1, 0x47, 0xC2}, 5},
    };
    std::vector<Fn> raw_fns;
    for (int pass = 0; pass < 2; ++pass) {
        for (Case& c : cases) {
            size_t off = 0;
            const Bytes site(c.raw, c.raw + c.raw_len);
            uint8_t* fn;
            if (c.raw_len == 6)
                fn = emit({{0x48, 0x83, 0xEC, 0x28, 0xC5, 0xFE, 0x6F, 0x12, 0xC5, 0xFE, 0x7F, 0x14, 0x24},
                           {0xC5, 0xFE, 0x6F, 0x09}, site, {0x48, 0x83, 0xC4, 0x28}, tail_y}, &off);
            else if (c.ymm)
                fn = emit({{0xC5, 0xFE, 0x6F, 0x09}, {0xC5, 0xFE, 0x6F, 0x12}, site, tail_y}, &off);
            else
                fn = emit({{0xC5, 0xFA, 0x6F, 0x09}, {0xC5, 0xFA, 0x6F, 0x12}, site, tail_x}, &off);
            if (pass == 0) {
                raw_fns.push_back(reinterpret_cast<Fn>(fn));
            } else {
                c.fn = reinterpret_cast<Fn>(fn);
                c.site = fn + off;
            }
        }
    }
    srandom(11);
    int k = 0;
    for (Case& c : cases) {
        VshiftCheckCounters* cnt = nullptr;
        CHECK(patch_vshift_check_site(reinterpret_cast<uint64_t>(c.site), 0x1000 + k, true, &cnt) == GTA6RT_OK);
        CHECK(cnt != nullptr && c.site[0] == 0xE8);
        if (!cnt) continue;
        const int lanes = c.ymm ? 4 : 2;
        uint64_t wrong = 0, high = 0, raw_wrong = 0;
        const int n = 4000;
        uint64_t last_bad[4] = {};
        for (int i = 0; i < n; ++i) {
            uint64_t src[4], count[4], out[4] = {}, raw_out[4] = {};
            bool any_high = false;
            for (int l = 0; l < 4; ++l) {
                src[l] = rnd();
                const int kind = static_cast<int>(random() % 4);
                count[l] = kind == 0 ? random() % 64 : kind == 1 ? 64 + random() % 1000
                         : kind == 2 ? (uint64_t(1 + random() % 7) << 32) | (random() % 64) : rnd();
                if (l < lanes && (count[l] >> 32)) any_high = true;
            }
            c.fn(src, count, out);
            raw_fns[k](src, count, raw_out);
            for (int l = 0; l < lanes; ++l) {
                wrong += out[l] != x86_shift(src[l], count[l], c.left);
                raw_wrong += raw_out[l] != x86_shift(src[l], count[l], c.left);
            }
            if (any_high) {
                ++high;
                std::memcpy(last_bad, count, sizeof last_bad);
            }
        }
        CHECK(wrong == 0);
        CHECK(cnt->executed == static_cast<uint64_t>(n));
        CHECK(cnt->high_count == high);
        CHECK(std::memcmp(cnt->last_bad, last_bad, lanes * 8) == 0);
        uint64_t src[4] = {1, 2, 3, 4}, count[4] = {1, 2, 3, 4}, out[4];
        const int reps = 2000000;
        uint64_t t0 = now_ns();
        for (int i = 0; i < reps; ++i) raw_fns[k](src, count, out);
        uint64_t t1 = now_ns();
        for (int i = 0; i < reps; ++i) c.fn(src, count, out);
        uint64_t t2 = now_ns();
        std::printf("%-28s %d runs, %llu with a lane >= 2^32; wrong lanes: raw Rosetta %llu, check stub %llu; call+raw %.1f ns, call+check stub %.1f ns\n",
                    c.name, n, (unsigned long long)high, (unsigned long long)raw_wrong, (unsigned long long)wrong, double(t1 - t0) / reps,
                    double(t2 - t1) / reps);
        ++k;
    }
    gta6rt_isa_vshift_check_report();
    std::printf("isa_vshift_check_test: %s, %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
