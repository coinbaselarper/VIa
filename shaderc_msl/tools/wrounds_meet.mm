// Stress probe: does a pairwise rendezvous of the two SIMD groups of one emulated wave64 (32-lane SIMD groups
// 2w and 2w+1 of a threadgroup, spinning on each other's threadgroup words) always finish on this GPU, and what does
// it cost against a threadgroup barrier per round? Every spin is bounded (a device-wide give-up flag plus a per-wait
// cap), so a deadlock shows up as counted failures, not as a hung command buffer.
//
//   wrounds_meet [--groups 64,256,...] [--tg 64,128,256] [--rounds 64] [--cap 65536] [--reps 5]
// Prints one line per case: MEET/BARRIER/SELF tg groups rounds: failures, data errors, ms (best of reps).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const char* kSrc = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct P { uint rounds; uint cap; uint mode; uint pad; };

kernel void meet(constant P& p [[buffer(0)]], device atomic_uint* stat [[buffer(1)]],
                 device uint* sink [[buffer(2)]],
                 uint li [[thread_index_in_threadgroup]], uint tgsz [[threads_per_threadgroup]],
                 uint sg [[simdgroup_index_in_threadgroup]], uint ln [[thread_index_in_simdgroup]],
                 uint gid [[threadgroup_position_in_grid]])
{
    threadgroup atomic_uint epoch[16];
    threadgroup uint data[16 * 32];
    if (li < 16) atomic_store_explicit(&epoch[li], 0u, memory_order_relaxed);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint partner = sg ^ 1u;
    uint acc = 0u, bad = 0u, fail = 0u;
    if (li != sg * 32u + ln) atomic_fetch_add_explicit(&stat[2], 1u, memory_order_relaxed);
    for (uint r = 1u; r <= p.rounds; ++r) {
        uint mine = gid * 7919u + r * 131u + li;
        if (p.mode == 0u) {
            data[sg * 32u + ln] = mine;
            atomic_thread_fence(mem_flags::mem_threadgroup, memory_order_seq_cst, thread_scope_threadgroup);
            if (ln == 0u) atomic_store_explicit(&epoch[sg], r, memory_order_relaxed);
            uint spins = 0u;
            while (atomic_load_explicit(&epoch[partner], memory_order_relaxed) < r) {
                if (++spins >= p.cap || (spins & 1023u) == 0u &&
                    atomic_load_explicit(&stat[3], memory_order_relaxed) != 0u) {
                    fail = 1u;
                    atomic_store_explicit(&stat[3], 1u, memory_order_relaxed);
                    break;
                }
            }
            atomic_thread_fence(mem_flags::mem_threadgroup, memory_order_seq_cst, thread_scope_threadgroup);
            uint want = gid * 7919u + r * 131u + (partner * 32u + ln);
            uint got = data[partner * 32u + ln];
            bad += (got != want && fail == 0u) ? 1u : 0u;
            acc += got;
            // both halves must be past this round's read before either overwrites its slot in the next round
            if (ln == 0u) atomic_store_explicit(&epoch[8u + sg], r, memory_order_relaxed);
            spins = 0u;
            while (atomic_load_explicit(&epoch[8u + partner], memory_order_relaxed) < r) {
                if (++spins >= p.cap || (spins & 1023u) == 0u &&
                    atomic_load_explicit(&stat[3], memory_order_relaxed) != 0u) {
                    fail = 1u;
                    atomic_store_explicit(&stat[3], 1u, memory_order_relaxed);
                    break;
                }
            }
        } else if (p.mode == 1u) {
            data[sg * 32u + ln] = mine;
            threadgroup_barrier(mem_flags::mem_threadgroup);
            uint want = gid * 7919u + r * 131u + (partner * 32u + ln);
            uint got = data[partner * 32u + ln];
            bad += got != want ? 1u : 0u;
            acc += got;
            threadgroup_barrier(mem_flags::mem_threadgroup);
        } else {
            // a lane waiting on another lane of its own SIMD group, in divergent code
            if (ln == 1u) atomic_store_explicit(&epoch[sg], r, memory_order_relaxed);
            if (ln == 0u) {
                uint spins = 0u;
                while (atomic_load_explicit(&epoch[sg], memory_order_relaxed) < r) {
                    if (++spins >= p.cap) { fail = 1u; break; }
                }
            }
        }
        if (fail) break;
    }
    if (bad) atomic_fetch_add_explicit(&stat[1], bad, memory_order_relaxed);
    if (fail && ln == 0u) atomic_fetch_add_explicit(&stat[0], 1u, memory_order_relaxed);
    if (acc == 0xdeadbeefu) sink[0] = acc;
}

kernel void meet_self(constant P& p [[buffer(0)]], device atomic_uint* stat [[buffer(1)]],
                      device uint* sink [[buffer(2)]], uint ln [[thread_index_in_simdgroup]])
{
    threadgroup atomic_uint w[1];
    if (ln == 0u) atomic_store_explicit(&w[0], 0u, memory_order_relaxed);
    simdgroup_barrier(mem_flags::mem_threadgroup);
    uint fail = 0u;
    if (ln == 0u) {
        uint spins = 0u;
        while (atomic_load_explicit(&w[0], memory_order_relaxed) == 0u) {
            if (++spins >= p.cap) { fail = 1u; break; }
        }
    } else if (ln == 1u) {
        atomic_store_explicit(&w[0], 1u, memory_order_relaxed);
    }
    if (fail) atomic_fetch_add_explicit(&stat[0], 1u, memory_order_relaxed);
}
)MSL";

static std::vector<int> ints(const char* s) {
    std::vector<int> v;
    std::string t(s);
    size_t i = 0;
    while (i < t.size()) {
        size_t j = t.find(',', i);
        if (j == std::string::npos) j = t.size();
        v.push_back(std::atoi(t.substr(i, j - i).c_str()));
        i = j + 1;
    }
    return v;
}

int main(int argc, char** argv) {
    @autoreleasepool {
        std::vector<int> groups = {64, 256, 1024, 4096, 16384, 65536}, tgs = {64, 128, 256};
        int rounds = 64, cap = 65536, reps = 5;
        for (int i = 1; i + 1 < argc; i += 2) {
            std::string a = argv[i];
            if (a == "--groups") groups = ints(argv[i + 1]);
            else if (a == "--tg") tgs = ints(argv[i + 1]);
            else if (a == "--rounds") rounds = std::atoi(argv[i + 1]);
            else if (a == "--cap") cap = std::atoi(argv[i + 1]);
            else if (a == "--reps") reps = std::atoi(argv[i + 1]);
        }
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError* err = nil;
        MTLCompileOptions* o = [MTLCompileOptions new];
        o.languageVersion = MTLLanguageVersion3_2;
        id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:kSrc] options:o error:&err];
        if (!lib) { std::fprintf(stderr, "compile: %s\n", err.localizedDescription.UTF8String); return 1; }
        id<MTLComputePipelineState> pm = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"meet"] error:&err];
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"meet_self"] error:&err];
        if (!pm || !ps) { std::fprintf(stderr, "pipeline: %s\n", err.localizedDescription.UTF8String); return 1; }
        std::printf("device %s, simd width %lu, max threads %lu\n", dev.name.UTF8String,
                    (unsigned long)pm.threadExecutionWidth, (unsigned long)pm.maxTotalThreadsPerThreadgroup);
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLBuffer> stat = [dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
        id<MTLBuffer> sink = [dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
        auto run = [&](id<MTLComputePipelineState> pso, uint32_t mode, int tg, int ng, int rd, double* ms) -> bool {
            uint32_t p[4] = {(uint32_t)rd, (uint32_t)cap, mode, 0};
            std::memset(stat.contents, 0, 64);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
            [e setComputePipelineState:pso];
            [e setBytes:p length:sizeof(p) atIndex:0];
            [e setBuffer:stat offset:0 atIndex:1];
            [e setBuffer:sink offset:0 atIndex:2];
            [e dispatchThreadgroups:MTLSizeMake(ng, 1, 1) threadsPerThreadgroup:MTLSizeMake(tg, 1, 1)];
            [e endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            if (cb.error) { std::printf("FAIL command buffer: %s\n", cb.error.localizedDescription.UTF8String); return false; }
            *ms = (cb.GPUEndTime - cb.GPUStartTime) * 1e3;
            return true;
        };
        for (int tg : tgs) {
            for (int ng : groups) {
                for (uint32_t mode : {0u, 1u}) {
                    double best = 1e30, ms = 0;
                    uint32_t fails = 0, bad = 0, unmapped = 0;
                    for (int r = 0; r < reps; ++r) {
                        if (!run(pm, mode, tg, ng, rounds, &ms)) return 3;
                        const uint32_t* s = (const uint32_t*)stat.contents;
                        fails += s[0];
                        bad += s[1];
                        unmapped += s[2];
                        if (ms < best) best = ms;
                        if (s[0]) break;
                    }
                    std::printf("%s\ttg %d\tgroups %d\trounds %d\tfailed waits %u\tdata errors %u\tlane map errors %u\t%.3f ms (best of %d)\t%.1f ns a round a group\n",
                                mode == 0 ? "MEET" : "BARRIER", tg, ng, rounds, fails, bad, unmapped, best, reps,
                                best * 1e6 / rounds / ng);
                    std::fflush(stdout);
                    if (mode == 0 && fails) {
                        std::printf("STOP: a pairwise wait gave up\n");
                        return 4;
                    }
                }
            }
        }
        double ms = 0;
        uint32_t p[4] = {1, (uint32_t)cap, 2, 0};
        (void)p;
        if (!run(ps, 2, 64, 64, 1, &ms)) return 3;
        std::printf("SELF\ttg 64\tgroups 64\tfailed waits %u of 128 SIMD groups\t%.3f ms\n",
                    ((const uint32_t*)stat.contents)[0], ms);
    }
    return 0;
}
