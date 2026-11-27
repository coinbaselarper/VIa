#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

static double d_of(uint64_t x) { double d; memcpy(&d, &x, 8); return d; }
static uint64_t u_of(double d) { uint64_t x; memcpy(&x, &d, 8); return x; }
static uint32_t uf(float f) { uint32_t x; memcpy(&x, &f, 4); return x; }

int main(int argc, char **argv) {
    @autoreleasepool {
        const char *pre = argc > 1 ? argv[1] : getenv("SHADERC_SOFT_F64");
        if (!pre) { printf("soft_f64_test: no soft_f64.metal path (SKIP)\n"); return 77; }
        std::ifstream f(pre);
        std::stringstream ss;
        ss << f.rdbuf();
        std::string src = "#include <metal_stdlib>\nusing namespace metal;\n" + ss.str() +
            "\nkernel void t(const device ulong2* in [[buffer(0)]], device ulong4* out [[buffer(1)]], uint i [[thread_position_in_grid]])\n"
            "{\n    spvSoftF64 a = spvSoftF64_from(in[i].x), b = spvSoftF64_from(in[i].y);\n"
            "    uint flags = uint(a < b) | (uint(a > b) << 1) | (uint(a <= b) << 2) | (uint(a == b) << 3) | (uint(isnan(a)) << 4) | (uint(isinf(a)) << 5);\n"
            "    out[i] = ulong4(spvFAdd(a, b).v, spvFSub(a, b).v, ulong(as_type<uint>(float(a))), ulong(flags));\n}\n";
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        MTLCompileOptions *co = [MTLCompileOptions new];
        co.languageVersion = MTLLanguageVersion3_1;
        id<MTLLibrary> lib = [dev newLibraryWithSource:@(src.c_str()) options:co error:&e];
        if (!lib) { printf("compile failed: %s\n", e.localizedDescription.UTF8String); return 1; }
        id<MTLComputePipelineState> p = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"t"] error:&e];
        if (!p) { printf("pso failed: %s\n", e.localizedDescription.UTF8String); return 1; }
        std::mt19937_64 rng(6464);
        std::vector<uint64_t> special = {0, 0x8000000000000000ull, 0x7FF0000000000000ull, 0xFFF0000000000000ull,
                                         0x7FF8000000000000ull, 0x0000000000000001ull, 0x8000000000000001ull,
                                         0x000FFFFFFFFFFFFFull, 0x0010000000000000ull, 0x7FEFFFFFFFFFFFFFull,
                                         0x3FF0000000000000ull, 0xBFF0000000000000ull, 0x47EFFFFFE0000000ull,
                                         0x47EFFFFFF0000000ull, 0x380FFFFFFFFFFFFFull, 0x36A0000000000000ull,
                                         0x36A0000000000001ull, 0x3690000000000000ull, 0x3810000000000000ull};
        const size_t N = 1 << 22;
        std::vector<uint64_t> in(2 * N);
        for (size_t i = 0; i < N; i++) {
            uint64_t a, b;
            int mode = (int)(i % 8);
            if (mode == 0) { a = special[rng() % special.size()]; b = special[rng() % special.size()]; }
            else if (mode == 1) { a = rng(); b = rng(); }
            else if (mode <= 4) {
                a = rng();
                int64_t de = (int64_t)(rng() % 130) - 65;
                uint64_t ea = (a >> 52) & 0x7FF;
                int64_t eb = (int64_t)ea + de;
                if (eb < 0) eb = 0;
                if (eb > 0x7FE) eb = 0x7FE;
                b = (rng() & 0x800FFFFFFFFFFFFFull) | ((uint64_t)eb << 52);
            } else if (mode == 5) {
                a = rng();
                b = (a ^ 0x8000000000000000ull) + (int64_t)(rng() % 64) - 32;
            } else if (mode == 6) {
                a = rng() & 0x801FFFFFFFFFFFFFull;
                b = rng() & 0x801FFFFFFFFFFFFFull;
            } else {
                double x = std::ldexp((double)(rng() >> 11), (int)(rng() % 400) - 250 - 53) * ((rng() & 1) ? -1 : 1);
                a = u_of(x);
                b = u_of(std::nextafter(-x, 0.0) * ((rng() & 1) ? 1.0 : -1.0));
            }
            in[2 * i] = a;
            in[2 * i + 1] = b;
        }
        id<MTLBuffer> bi = [dev newBufferWithBytes:in.data() length:in.size() * 8 options:MTLResourceStorageModeShared];
        id<MTLBuffer> bo = [dev newBufferWithLength:N * 32 options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:p];
        [ce setBuffer:bi offset:0 atIndex:0];
        [ce setBuffer:bo offset:0 atIndex:1];
        [ce dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        [ce endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        const uint64_t *o = (const uint64_t *)bo.contents;
        size_t bad_add = 0, bad_sub = 0, bad_cvt = 0, bad_cmp = 0, nan_cases = 0, sub_results = 0;
        for (size_t i = 0; i < N; i++) {
            double a = d_of(in[2 * i]), b = d_of(in[2 * i + 1]);
            double s = a + b, d = a - b;
            float c = (float)a;
            auto same = [](uint64_t g, double w) { return std::isnan(w) ? std::isnan(d_of(g)) : g == u_of(w); };
            if (!same(o[4 * i], s)) { if (bad_add < 5) printf("add %016llx + %016llx: got %016llx want %016llx\n", (unsigned long long)in[2*i], (unsigned long long)in[2*i+1], (unsigned long long)o[4*i], (unsigned long long)u_of(s)); bad_add++; }
            if (!same(o[4 * i + 1], d)) bad_sub++;
            uint32_t gc = (uint32_t)o[4 * i + 2];
            if (std::isnan(c) ? !std::isnan(d_of(0) + [&] { float t; memcpy(&t, &gc, 4); return (double)t; }()) : gc != uf(c)) bad_cvt++;
            uint32_t fl = (uint32_t)o[4 * i + 3];
            uint32_t want = (a < b) | ((a > b) << 1) | ((a <= b) << 2) | ((a == b) << 3) | (std::isnan(a) << 4) | (std::isinf(a) << 5);
            if (fl != want) bad_cmp++;
            if (std::isnan(s) || std::isnan(d)) nan_cases++;
            if (s != 0 && std::fpclassify(s) == FP_SUBNORMAL) sub_results++;
        }
        double gpu_ms = (cb.GPUEndTime - cb.GPUStartTime) * 1e3;
        printf("soft_f64_test: %zu cases (%zu NaN results, %zu subnormal sums): add mismatches %zu, sub %zu, f64->f32 %zu, "
               "compare/isnan/isinf %zu; GPU %.2f ms for %zu x (add, sub, convert, 4 compares)\n",
               N, nan_cases, sub_results, bad_add, bad_sub, bad_cvt, bad_cmp, gpu_ms, N);
        return (bad_add || bad_sub || bad_cvt || bad_cmp) ? 1 : 0;
    }
}
