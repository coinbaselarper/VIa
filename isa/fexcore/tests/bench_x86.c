// Rosetta side of the guest-speed comparison: the kernels of fex_guest.S linked into an x86_64 executable and
// called directly (Win64 ABI), so Rosetta translates exactly the bytes FEX runs in isa_fex_test --bench.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>

#define MS __attribute__((ms_abi))
MS uint64_t g_bench_int(uint64_t n, uint64_t seed);
MS uint64_t g_bench_mix(uint64_t n);
MS uint64_t g_bench_mem(uint64_t* a, uint64_t reps, uint64_t len);
MS double g_bench_sse(const float* a, const float* b, uint64_t reps, uint64_t len);
MS uint32_t g_bench_avx2(const int32_t* a, const int32_t* b, uint64_t reps, uint64_t len);
MS uint64_t g_bench_calls(void* thunk, uint64_t n);
MS uint64_t g_add3(uint64_t a, uint64_t b, uint64_t c);

static MS uint64_t count_fn(uint64_t i) {
    return i & 1;
}

static uint64_t now_ns(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

#define BEST(var, stmt)                              \
    do {                                             \
        var = 1e300;                                 \
        for (int r_ = 0; r_ < 5; ++r_) {             \
            uint64_t t0_ = now_ns();                 \
            stmt;                                    \
            double d_ = (double)(now_ns() - t0_);    \
            if (d_ < var) var = d_;                  \
        }                                            \
    } while (0)

int main(void) {
    int tr = 0;
    size_t ts = sizeof tr;
    sysctlbyname("sysctl.proc_translated", &tr, &ts, NULL, 0);
    printf("isa_fex_bench_x86: proc_translated=%d\n\n", tr);
    printf("| kernel | Rosetta ns/iter | checksum |\n|---|---:|---|\n");
    const uint64_t kInt = 100000000, kMix = 50000000, kLen = 4096, kReps = 5000, kCalls = 5000000;
    double g;
    volatile uint64_t sink = 0;
    uint64_t r = 0;
    g_bench_int(1000, 1);
    BEST(g, r = g_bench_int(kInt, 1));
    printf("| int LCG chain | %.3f | 0x%llx |\n", g / kInt, (unsigned long long)r);
    BEST(g, r = g_bench_mix(kMix));
    printf("| branchy mix + call | %.3f | 0x%llx |\n", g / kMix, (unsigned long long)r);
    uint64_t* mem = calloc(kLen, 8);
    float* fa = calloc(kLen, 4);
    float* fb = calloc(kLen, 4);
    int32_t* ia = calloc(kLen, 4);
    int32_t* ib = calloc(kLen, 4);
    for (uint64_t i = 0; i < kLen; ++i) {
        mem[i] = i;
        fa[i] = 1.0f + (float)(i % 7) * 1e-3f;
        fb[i] = 1.0f - (float)(i % 5) * 1e-3f;
        ia[i] = (int32_t)(i * 2654435761u);
        ib[i] = (int32_t)(i | 1);
    }
    BEST(g, r = g_bench_mem(mem, kReps, kLen));
    printf("| load/store chain | %.3f | - |\n", g / (kLen * kReps));
    double dr = 0;
    BEST(g, dr = g_bench_sse(fa, fb, kReps, kLen));
    printf("| SSE mulps/addps (per 8 floats) | %.3f | %f |\n", g / (kLen / 8 * kReps), dr);
    uint32_t ur = 0;
    BEST(g, ur = g_bench_avx2(ia, ib, kReps, kLen));
    printf("| AVX2 vpmulld/vpaddd (per 8 ints) | %.3f | 0x%x |\n", g / (kLen / 8 * kReps), ur);
    BEST(g, r = g_bench_calls((void*)count_fn, kCalls));
    printf("| guest->host call+return (direct ms_abi call) | %.1f | sum %llu |\n", g / kCalls, (unsigned long long)r);
    BEST(g, for (int i = 0; i < 200000; ++i) sink += g_add3(1, 2, 3));
    printf("| host->guest call+return (direct call) | %.1f | - |\n", g / 200000);
    (void)sink;
    return 0;
}
