#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

struct Shader {
    std::string stem, dir, tier, ops;
    int push_lane = 0, push_kernel = 0;
    std::vector<std::pair<int, std::string>> attrs;
};

static std::string slurp(const std::string &p) {
    std::ifstream f(p);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

static MTLVertexFormat vfmt(const std::string &t) {
    static const std::map<std::string, MTLVertexFormat> m = {
        {"float", MTLVertexFormatFloat}, {"float2", MTLVertexFormatFloat2}, {"float3", MTLVertexFormatFloat3},
        {"float4", MTLVertexFormatFloat4}, {"uint", MTLVertexFormatUInt}, {"uint2", MTLVertexFormatUInt2},
        {"uint3", MTLVertexFormatUInt3}, {"uint4", MTLVertexFormatUInt4}, {"int", MTLVertexFormatInt},
        {"int2", MTLVertexFormatInt2}, {"int3", MTLVertexFormatInt3}, {"int4", MTLVertexFormatInt4},
        {"half", MTLVertexFormatHalf}, {"half2", MTLVertexFormatHalf2}, {"half3", MTLVertexFormatHalf3},
        {"half4", MTLVertexFormatHalf4}};
    auto it = m.find(t);
    return it == m.end() ? MTLVertexFormatInvalid : it->second;
}

static uint32_t mix(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

static uint32_t pattern(uint32_t i) { return (0x3F00u + (mix(i) % 0xC0u)) << 16; }

static const uint64_t kHeap = 0x48000000ull;

struct Run {
    bool ok = false;
    std::string error;
    double gpu_ms = 0;
};

static Run finish(id<MTLCommandBuffer> cb) {
    [cb commit];
    [cb waitUntilCompleted];
    Run r;
    r.ok = cb.status == MTLCommandBufferStatusCompleted;
    if (!r.ok) r.error = cb.error ? cb.error.localizedDescription.UTF8String : "command buffer failed";
    r.gpu_ms = (cb.GPUEndTime - cb.GPUStartTime) * 1e3;
    return r;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);
        const char *man = argc > 1 ? argv[1] : getenv("SHADERC_VS_LANE_MANIFEST");
        if (!man) { printf("vs_lane_test: no manifest (SKIP)\n"); return 77; }
        std::ifstream mf(man);
        if (!mf) { printf("vs_lane_test: cannot open %s (SKIP)\n", man); return 77; }
        std::vector<Shader> shaders;
        std::map<std::string, size_t> by;
        std::string line;
        while (std::getline(mf, line)) {
            std::istringstream ls(line);
            std::string kind;
            ls >> kind;
            if (kind == "shader") {
                Shader s;
                ls >> s.stem >> s.dir >> s.tier >> s.push_lane >> s.push_kernel >> s.ops;
                by[s.stem] = shaders.size();
                shaders.push_back(s);
            } else if (kind == "attr") {
                std::string stem, t;
                int idx;
                ls >> stem >> idx >> t;
                shaders[by[stem]].attrs.push_back({idx, t});
            }
        }
        int expect_equal = -1;
        for (int i = 2; i < argc; i++)
            if (!strncmp(argv[i], "--expect-equal=", 15)) expect_equal = atoi(argv[i] + 15);
        int limit = getenv("SHADERC_VS_LANE_LIMIT") ? atoi(getenv("SHADERC_VS_LANE_LIMIT")) : 0;
        size_t start = getenv("SHADERC_VS_LANE_START") ? (size_t)atol(getenv("SHADERC_VS_LANE_START")) : 0;
        bool stop_on_gpu_error = getenv("SHADERC_VS_LANE_START") != nullptr;
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLBuffer> heap = [dev newBufferWithLength:kHeap options:MTLResourceStorageModeShared];
        uint32_t *hp = (uint32_t *)heap.contents;
        const char *heap_int = getenv("SHADERC_VS_LANE_HEAP_INT");
        for (uint64_t i = 0; i < kHeap / 4; i++) hp[i] = heap_int ? (uint32_t)strtoul(heap_int, nullptr, 0) : pattern((uint32_t)i);
        uint64_t heap_va = heap.gpuAddress;
        uint32_t push[36];
        for (int i = 0; i < 32; i++) push[i] = 0x01000000u + 0x40u * (uint32_t)i;
        push[32] = (uint32_t)heap_va; push[33] = (uint32_t)(heap_va >> 32);
        push[34] = (uint32_t)heap_va; push[35] = (uint32_t)(heap_va >> 32);
        MTLCompileOptions *co = [MTLCompileOptions new];
        co.languageVersion = MTLLanguageVersion3_1;
        id<MTLLibrary> stubl = [dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nfragment float4 stub_fs() { return float4(0); }\n"
                                                 options:co error:nil];
        MTLTextureDescriptor *rtd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                       width:64 height:64 mipmapped:NO];
        rtd.usage = MTLTextureUsageRenderTarget;
        rtd.storageMode = MTLStorageModePrivate;
        id<MTLTexture> rt = [dev newTextureWithDescriptor:rtd];
        int tested = 0, unexplained = 0, clip_copy_only = 0, lane_sensitive = 0, mutant_detected = 0, shuffled_diff = 0, equal_all = 0, faulted = 0, compile_fail = 0, mismatched = 0;
        size_t n_done = 0;
        double lane_ms = 0, kern_ms = 0, pass_ms = 0;
        for (size_t si = start; si < shaders.size(); si++) {
            auto &s = shaders[si];
            if (limit && (int)n_done >= limit) break;
            n_done++;
            NSError *e = nil;
            std::string lane_src = slurp(s.dir + "/lane.metal"), kern_src = slurp(s.dir + "/kernel.metal");
            id<MTLLibrary> ll = [dev newLibraryWithSource:@(lane_src.c_str()) options:co error:&e];
            id<MTLLibrary> kl = ll ? [dev newLibraryWithSource:@(kern_src.c_str()) options:co error:&e] : nil;
            std::string mut_src = slurp(s.dir + "/lane_mut.metal");
            id<MTLLibrary> ml = mut_src.empty() ? nil : [dev newLibraryWithSource:@(mut_src.c_str()) options:co error:nil];
            if (!ll || !kl) {
                printf("shader %s COMPILE_FAIL %s\n", s.stem.c_str(), e.localizedDescription.UTF8String);
                compile_fail++;
                continue;
            }
            const uint32_t stride = 16 * (uint32_t)std::max<size_t>(1, s.attrs.size() ? (size_t)(s.attrs.back().first + 1) : 1);
            MTLVertexDescriptor *vd = [MTLVertexDescriptor new];
            MTLStageInputOutputDescriptor *sd = [MTLStageInputOutputDescriptor stageInputOutputDescriptor];
            for (auto &a : s.attrs) {
                vd.attributes[a.first].format = vfmt(a.second);
                vd.attributes[a.first].offset = 16 * a.first;
                vd.attributes[a.first].bufferIndex = 30;
                sd.attributes[a.first].format = (MTLAttributeFormat)vfmt(a.second);
                sd.attributes[a.first].offset = 16 * a.first;
                sd.attributes[a.first].bufferIndex = 30;
            }
            vd.layouts[30].stride = stride;
            sd.layouts[30].stride = stride;
            sd.layouts[30].stepFunction = MTLStepFunctionThreadPositionInGridX;
            MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
            rd.vertexFunction = [ll newFunctionWithName:@"main0"];
            rd.fragmentFunction = [stubl newFunctionWithName:@"stub_fs"];
            rd.vertexDescriptor = vd;
            rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            MTLRenderPipelineReflection *refl = nil;
            id<MTLRenderPipelineState> rp = [dev newRenderPipelineStateWithDescriptor:rd
                options:MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo reflection:&refl error:&e];
            id<MTLRenderPipelineState> mp = nil;
            if (ml) {
                rd.vertexFunction = [ml newFunctionWithName:@"main0"];
                mp = [dev newRenderPipelineStateWithDescriptor:rd error:nil];
                rd.vertexFunction = [ll newFunctionWithName:@"main0"];
            }
            MTLComputePipelineDescriptor *cd = [MTLComputePipelineDescriptor new];
            cd.computeFunction = [kl newFunctionWithName:@"main0"];
            cd.stageInputDescriptor = sd;
            MTLComputePipelineReflection *krefl = nil;
            id<MTLComputePipelineState> cp = rp ? [dev newComputePipelineStateWithDescriptor:cd
                options:MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo reflection:&krefl error:&e] : nil;
            if (!rp || !cp) {
                printf("shader %s PSO_FAIL %s\n", s.stem.c_str(), e.localizedDescription.UTF8String);
                compile_fail++;
                continue;
            }
            size_t out_size = 0, pos_off = SIZE_MAX;
            std::vector<size_t> clip_copy_words;
            for (id<MTLBinding> b in refl.vertexBindings) {
                if ([b.name isEqualToString:@"spvCapture"]) {
                    id<MTLBufferBinding> bb = (id<MTLBufferBinding>)b;
                    out_size = bb.bufferDataSize;
                    for (MTLStructMember *m in bb.bufferPointerType.elementStructType.members) {
                        if ([m.name isEqualToString:@"gl_Position"]) pos_off = m.offset;
                        if ([m.name hasPrefix:@"gl_ClipDistance_"] || [m.name hasPrefix:@"gl_CullDistance_"]) clip_copy_words.push_back(m.offset / 4);
                    }
                }
            }
            if (!out_size) { printf("shader %s NO_CAPTURE_SIZE\n", s.stem.c_str()); compile_fail++; continue; }
            auto members = [](id<MTLBufferBinding> bb, size_t total) {
                std::vector<std::pair<std::string, std::pair<size_t, size_t>>> v;
                std::vector<std::pair<size_t, std::string>> by;
                for (MTLStructMember *m in bb.bufferPointerType.elementStructType.members) by.push_back({m.offset, m.name.UTF8String});
                std::sort(by.begin(), by.end());
                for (size_t i = 0; i < by.size(); i++)
                    v.push_back({by[i].second, {by[i].first, (i + 1 < by.size() ? by[i + 1].first : total) - by[i].first}});
                return v;
            };
            std::vector<std::pair<std::string, std::pair<size_t, size_t>>> lane_m, kern_m;
            size_t kout_size = 0;
            for (id<MTLBinding> b in refl.vertexBindings)
                if ([b.name isEqualToString:@"spvCapture"]) lane_m = members((id<MTLBufferBinding>)b, out_size);
            for (id<MTLBinding> b in krefl.bindings)
                if ([b.name isEqualToString:@"spvOut"]) {
                    kout_size = ((id<MTLBufferBinding>)b).bufferDataSize;
                    kern_m = members((id<MTLBufferBinding>)b, kout_size);
                }
            if (!kout_size) { printf("shader %s NO_KERNEL_OUT_SIZE\n", s.stem.c_str()); compile_fail++; continue; }
            std::vector<std::pair<size_t, size_t>> copy_map;
            std::vector<size_t> lane_only_words;
            for (auto &lm : lane_m) {
                bool found = false;
                for (auto &km : kern_m)
                    if (km.first == lm.first) {
                        size_t n = std::min(lm.second.second, km.second.second);
                        for (size_t b = 0; b + 4 <= n; b += 4) copy_map.push_back({lm.second.first + b, km.second.first + b});
                        found = true;
                    }
                if (!found)
                    for (size_t b = 0; b + 4 <= lm.second.second; b += 4) lane_only_words.push_back((lm.second.first + b) / 4);
            }
            const uint32_t N = getenv("SHADERC_VS_LANE_N") ? (uint32_t)atoi(getenv("SHADERC_VS_LANE_N")) : 3000;
            const bool big = getenv("SHADERC_VS_LANE_NOBIG") == nullptr;
            id<MTLBuffer> vb = [dev newBufferWithLength:(NSUInteger)stride * N options:MTLResourceStorageModeShared];
            uint8_t *vp = (uint8_t *)vb.contents;
            for (uint32_t v = 0; v < N; v++)
                for (auto &a : s.attrs)
                    for (int c = 0; c < 4; c++) {
                        uint32_t *d = (uint32_t *)(vp + (size_t)v * stride + 16 * a.first + 4 * c);
                        uint32_t h = mix(v * 131u + (uint32_t)a.first * 17u + (uint32_t)c);
                        if (a.second.rfind("float", 0) == 0) *d = (0x3F00u + h % 0xC0u) << 16;
                        else if (a.second.rfind("half", 0) == 0) *d = 0x3C003C00u ^ (h & 0x01FF01FFu);
                        else *d = h % 64u;
                    }
            auto lane_run = [&](id<MTLBuffer> cap, id<MTLBuffer> ib, uint32_t count, id<MTLRenderPipelineState> pso = nil) {
                memset(cap.contents, 0xCD, cap.length);
                id<MTLCommandBuffer> cb = [q commandBuffer];
                MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
                p.colorAttachments[0].texture = rt;
                p.colorAttachments[0].loadAction = MTLLoadActionClear;
                p.colorAttachments[0].storeAction = MTLStoreActionDontCare;
                id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
                [re setRenderPipelineState:pso ? pso : rp];
                [re useResource:heap usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
                [re setVertexBytes:push length:144 atIndex:s.push_lane];
                [re setVertexBuffer:vb offset:0 atIndex:30];
                [re setVertexBuffer:cap offset:0 atIndex:28];
                if (ib) [re drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:count indexType:MTLIndexTypeUInt32 indexBuffer:ib indexBufferOffset:0];
                else [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:count];
                [re endEncoding];
                return finish(cb);
            };
            auto kern_run = [&](id<MTLBuffer> cap, id<MTLBuffer> verts, uint32_t count, NSUInteger tg) {
                memset(cap.contents, 0, cap.length);
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                [ce setComputePipelineState:cp];
                [ce useResource:heap usage:MTLResourceUsageRead];
                [ce setBytes:push length:144 atIndex:s.push_kernel];
                [ce setBuffer:verts offset:0 atIndex:30];
                [ce setBuffer:cap offset:0 atIndex:28];
                [ce setStageInRegion:MTLRegionMake1D(0, count)];
                [ce dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(tg, 1, 1)];
                [ce endEncoding];
                return finish(cb);
            };
            id<MTLBuffer> capL = [dev newBufferWithLength:out_size * N options:MTLResourceStorageModeShared];
            id<MTLBuffer> capL2 = [dev newBufferWithLength:out_size * N options:MTLResourceStorageModeShared];
            id<MTLBuffer> capS = [dev newBufferWithLength:out_size * N options:MTLResourceStorageModeShared];
            id<MTLBuffer> capK = [dev newBufferWithLength:std::max(out_size, kout_size) * N options:MTLResourceStorageModeShared];
            id<MTLBuffer> capK32 = [dev newBufferWithLength:std::max(out_size, kout_size) * N options:MTLResourceStorageModeShared];
            id<MTLBuffer> ib = [dev newBufferWithLength:4 * N options:MTLResourceStorageModeShared];
            uint32_t *ip = (uint32_t *)ib.contents;
            for (uint32_t i = 0; i < N; i++) ip[i] = i;
            for (uint32_t i = N - 1; i > 0; i--) { uint32_t j = mix(i) % (i + 1); std::swap(ip[i], ip[j]); }
            Run a = lane_run(capL, nil, N), a2 = lane_run(capL2, nil, N), sh = lane_run(capS, ib, N);
            Run k = kern_run(capK, vb, N, 64), k32 = kern_run(capK32, vb, N, 32);
            auto relayout = [&](id<MTLBuffer> cap) {
                std::vector<uint8_t> tmp((uint8_t *)cap.contents, (uint8_t *)cap.contents + kout_size * N);
                memset(cap.contents, 0, cap.length);
                for (uint32_t v = 0; v < N; v++)
                    for (auto &cm : copy_map) memcpy((uint8_t *)cap.contents + v * out_size + cm.first, tmp.data() + v * kout_size + cm.second, 4);
            };
            relayout(capK);
            relayout(capK32);
            tested++;
            if (!a.ok || !k.ok || !a2.ok || !sh.ok || !k32.ok) {
                printf("shader %s GPU_ERROR lane=%s kernel=%s\n", s.stem.c_str(), a.ok ? "ok" : a.error.c_str(), k.ok ? "ok" : k.error.c_str());
                faulted++;
                if (stop_on_gpu_error) { printf("stop_at %zu\n", si); return 3; }
                continue;
            }
            std::vector<bool> masked(out_size / 4, false);
            for (size_t w : clip_copy_words) if (w < masked.size()) masked[w] = true;
            for (size_t w : lane_only_words) if (w < masked.size()) masked[w] = true;
            auto diff_masked = [&](id<MTLBuffer> x, id<MTLBuffer> y, bool mask) {
                int n = 0;
                const uint32_t *a = (const uint32_t *)x.contents, *b = (const uint32_t *)y.contents;
                for (uint32_t v = 0; v < N; v++)
                    for (size_t w = 0; w < out_size / 4; w++)
                        if ((!mask || !masked[w]) && a[v * out_size / 4 + w] != b[v * out_size / 4 + w]) { n++; break; }
                return n;
            };
            auto diff = [&](id<MTLBuffer> x, id<MTLBuffer> y) { return diff_masked(x, y, false); };
            auto untouched = [&](id<MTLBuffer> x) {
                int n = 0;
                std::vector<uint8_t> cd(out_size, 0xCD);
                for (uint32_t v = 0; v < N; v++)
                    if (!memcmp((uint8_t *)x.contents + v * out_size, cd.data(), out_size)) n++;
                return n;
            };
            int d_mut = -1;
            if (mp) {
                id<MTLBuffer> capM = [dev newBufferWithLength:out_size * N options:MTLResourceStorageModeShared];
                Run mr = lane_run(capM, nil, N, mp);
                d_mut = mr.ok ? diff(capM, capL) : -2;
                if (d_mut > 0) mutant_detected++;
            }
            if (const char *dump = getenv("SHADERC_VS_LANE_DUMP")) {
                std::string base = std::string(dump) + "/" + std::to_string(si);
                FILE *f = fopen((base + ".lane.bin").c_str(), "wb"); fwrite(capL.contents, 1, out_size * N, f); fclose(f);
                f = fopen((base + ".kernel.bin").c_str(), "wb"); fwrite(capK.contents, 1, out_size * N, f); fclose(f);
            }
            int d_lk_raw = diff(capL, capK);
            int d_lk = diff_masked(capL, capK, true), d_ll = diff(capL, capL2), d_ls = diff_masked(capS, capK, true), d_kk = diff(capK, capK32);
            int d_pos = 0;
            if (pos_off != SIZE_MAX)
                for (uint32_t v = 0; v < N; v++)
                    if (memcmp((uint8_t *)capL.contents + v * out_size + pos_off, (uint8_t *)capK.contents + v * out_size + pos_off, 16)) d_pos++;
            int nz = 0;
            for (uint32_t v = 0; v < N; v++) {
                const uint8_t *p = (const uint8_t *)capK.contents + v * out_size;
                for (size_t b = 0; b < out_size; b++) if (p[b]) { nz++; break; }
            }
            const uint32_t NB = big ? 3 * 87382 : 3;
            id<MTLBuffer> vbB = [dev newBufferWithLength:(NSUInteger)stride * NB options:MTLResourceStorageModeShared];
            for (uint32_t v = 0; v < NB; v++) memcpy((uint8_t *)vbB.contents + (size_t)v * stride, vp + (size_t)(v % N) * stride, stride);
            id<MTLBuffer> capB = [dev newBufferWithLength:std::max(out_size, kout_size) * NB options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
            p.colorAttachments[0].texture = rt;
            p.colorAttachments[0].loadAction = MTLLoadActionClear;
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
            [re setRenderPipelineState:rp];
            [re useResource:heap usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
            [re setVertexBytes:push length:144 atIndex:s.push_lane];
            [re setVertexBuffer:vbB offset:0 atIndex:30];
            [re setVertexBuffer:capB offset:0 atIndex:28];
            [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:NB];
            [re endEncoding];
            Run tl = finish(cb);
            Run tk = kern_run(capB, vbB, NB, 64);
            static id<MTLRenderPipelineState> passp;
            if (!passp && pos_off != SIZE_MAX) {
                NSString *src = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\nvertex float4 pass_vs(uint v [[vertex_id]], const device uchar* o [[buffer(0)]], constant uint2& so [[buffer(1)]]) { return *(const device packed_float4*)(o + v * so.x + so.y); }\nfragment float4 pass_fs() { return float4(0); }\n"];
                id<MTLLibrary> pl = [dev newLibraryWithSource:src options:co error:&e];
                MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
                pd.vertexFunction = [pl newFunctionWithName:@"pass_vs"];
                pd.fragmentFunction = [pl newFunctionWithName:@"pass_fs"];
                pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
                passp = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
            }
            Run tp;
            if (passp) {
                uint32_t so[2] = {(uint32_t)out_size, (uint32_t)pos_off};
                id<MTLCommandBuffer> cb2 = [q commandBuffer];
                id<MTLRenderCommandEncoder> re2 = [cb2 renderCommandEncoderWithDescriptor:p];
                [re2 setRenderPipelineState:passp];
                [re2 setVertexBuffer:capB offset:0 atIndex:0];
                [re2 setVertexBytes:so length:8 atIndex:1];
                [re2 drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:NB];
                [re2 endEncoding];
                tp = finish(cb2);
            }
            std::string words;
            if (d_lk) {
                std::vector<int> wd(out_size / 4, 0);
                for (uint32_t v = 0; v < N; v++)
                    for (size_t w = 0; w < out_size / 4; w++)
                        if (((uint32_t *)capL.contents)[v * out_size / 4 + w] != ((uint32_t *)capK.contents)[v * out_size / 4 + w]) wd[w]++;
                for (size_t w = 0; w < wd.size(); w++)
                    if (wd[w]) words += " +" + std::to_string(4 * w) + ":" + std::to_string(wd[w]);
                printf("shader %s differing words (byte offset:vertices)%s\n", s.stem.c_str(), words.c_str());
            }
            bool eq = d_lk == 0;
            if (d_lk_raw != d_lk) clip_copy_only++;
            if (d_mut > 0) lane_sensitive++;
            if (d_ll || d_kk || d_lk) unexplained++;
            if (d_ls) shuffled_diff++;
            if (eq) equal_all++; else mismatched++;
            lane_ms += tl.gpu_ms; kern_ms += tk.gpu_ms; pass_ms += tp.gpu_ms;
            printf("shader %s tier %s ops %s out_bytes %zu N %u nonzero %d lane_vs_kernel %d lane_repeat %d lane_shuffled_vs_kernel %d "
                   "kernel_tg64_vs_tg32 %d mutant_vs_lane %d unmasked_lane_vs_kernel %d kernel_out_bytes %zu lane_only_words %zu position_diff %d untouched_lane %d | %u verts: lane %.3f ms, kernel %.3f ms | %u verts: lane %.3f ms, kernel %.3f ms + passthrough %.3f ms\n",
                   s.stem.c_str(), s.tier.c_str(), s.ops.c_str(), out_size, N, nz, d_lk, d_ll, d_ls, d_kk, d_mut, d_lk_raw, kout_size, lane_only_words.size(), d_pos, untouched(capL),
                   N, a.gpu_ms, k.gpu_ms, NB, tl.gpu_ms, tk.gpu_ms, tp.gpu_ms);
        }
        printf("vs_lane_test: %d shaders run, %d lane == kernel, %d differ, %d differ only when shuffled, %d where the reversed-lane mutant differs, %d GPU errors, %d compile/pipeline failures; "
               "GPU time over the large draws: lane %.2f ms, kernel %.2f ms + passthrough %.2f ms\n",
               tested, equal_all, mismatched, shuffled_diff, mutant_detected, faulted, compile_fail, lane_ms, kern_ms, pass_ms);
        printf("vs_lane_test: %d lane-sensitive (reversed-lane mutant changes the lane route's output), %d differ only in the "
               "user(clipN)/user(cullN) copies the kernel route leaves unwritten\n", lane_sensitive, clip_copy_only);
        printf("vs_lane_test: %d unexplained (repeat differs, tg64 != tg32, or lane != kernel outside the clip/cull copies)\n", unexplained);
        printf("vs_lane_test: Metal API validation %s, shader validation %s\n", getenv("MTL_DEBUG_LAYER") ? "on" : "off",
               getenv("MTL_SHADER_VALIDATION") ? "on" : "off");
        if (tested - faulted <= 0) return 1;
        if (expect_equal >= 0) {
            bool ok = tested == expect_equal && equal_all == expect_equal && !faulted && !compile_fail && !mismatched && !unexplained;
            printf("vs_lane_test: expected %d of %d lane == kernel with 0 GPU errors: %s\n", expect_equal, expect_equal, ok ? "OK" : "FAIL");
            return ok ? 0 : 1;
        }
        return unexplained ? 1 : 0;
    }
}
