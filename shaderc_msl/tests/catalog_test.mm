#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <regex>
#include <string>
#include <vector>

#include "gta6rt_shaderc_msl/catalog.h"

using gta6rt::shaderc_msl::Catalog;
using gta6rt::shaderc_msl::ShaderEntry;

static const char *kPairs[][3] = {
    {"blit", "VS_Blit_fad9fd10_Wrapped", "PS_BlitMod_fad9fd10_Wrapped"},
    {"ui_text", "VInstancedText_27eac676_Wrapped", "FTextMul_27eac676_Wrapped"},
    {"ui_gradient", "_VInstancedPosition3dVertexCxformGradientMask_15356630_Wrapped", ".FVertexCxformMulGradientMask_15356630_Wrapped"},
    {"bink", "VS_Bink_e9aa19fd_Wrapped", "PS_BinkNoAlpha_e9aa19fd_Wrapped"},
    {"ui_hash_ps", "%VBatchTexTGCxformAc_d17d2c71_Wrapped", "#FSColor2FullBevel_d17d2c71_Wrapped"},
    {"ui_hash_vs_ps", "#VBatchTexTGCxform_eca9e4f0_Wrapped", "#FTexTGCxformAcMul_eca9e4f0_Wrapped"},
};
static const int kNumPairs = sizeof(kPairs) / sizeof(kPairs[0]);

struct Row {
    std::string name, key, stage;
};

static std::vector<Row> data_rows(const char *path) {
    std::vector<Row> rows;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        auto t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        auto t2 = line.find('\t', t1 + 1), t3 = line.find('\t', t2 + 1);
        rows.push_back({line.substr(0, t1), line.substr(t1 + 1, t2 - t1 - 1), line.substr(t2 + 1, t3 - t2 - 1)});
    }
    return rows;
}

static MTLVertexFormat fmt(MTLDataType t) {
    switch (t) {
        case MTLDataTypeFloat: return MTLVertexFormatFloat;
        case MTLDataTypeFloat2: return MTLVertexFormatFloat2;
        case MTLDataTypeFloat3: return MTLVertexFormatFloat3;
        case MTLDataTypeUInt: return MTLVertexFormatUInt;
        case MTLDataTypeUInt2: return MTLVertexFormatUInt2;
        case MTLDataTypeUInt4: return MTLVertexFormatUInt4;
        case MTLDataTypeInt4: return MTLVertexFormatInt4;
        default: return MTLVertexFormatFloat4;
    }
}

static bool synthetic_ok() {
    std::string path = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") + "/shaderc_msl_catalog_synthetic.tsv";
    {
        std::ofstream f(path);
        f << "# shaderc_msl manifest v2 root=lib\n# name key stage entry metallib push tables\n"
          << "#FHash_1\tk1\tps\tfragment\tcas/a.metallib\t4\t0:1,1:2\t0\t-\t-\t-\n"
          << "# space\tk2\tvs\tvertex\tcas/b.metallib\t-\t-\t1\tlane\t-\t-\n"
          << "PlainName\tk3\tms\tmesh\tcas/c.metallib\t-\t-\t0\t-\t3:7\thw\n";
    }
    Catalog c;
    std::string err;
    bool ok = c.load(path, &err) && c.size() == 3;
    const ShaderEntry *a = c.find("#FHash_1"), *b = c.find("# space"), *p = c.find("PlainName");
    ok = ok && a && a->key == "k1" && a->push_buffer == 4 && a->tables.size() == 2;
    ok = ok && b && b->vertex_as_kernel && b->push_buffer == -1;
    ok = ok && p && p->mesh_route == "hw" && p->descriptor_arrays.size() == 1;
    ok = ok && c.root().size() >= 4 && c.root().compare(c.root().size() - 4, 4, "/lib") == 0;
    printf("catalog synthetic: %zu of 3 rows loaded ('#'-prefixed names included): %s\n", c.size(), ok ? "OK" : "FAIL");
    remove(path.c_str());
    return ok;
}

static bool synthetic_v3_ok() {
    std::string path = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") + "/shaderc_msl_catalog_v3.tsv";
    {
        std::ofstream f(path);
        auto row = [](std::vector<std::string> v) {
            v.resize(29, "-");
            std::string s;
            for (size_t i = 0; i < v.size(); i++) s += (i ? "\t" : "") + v[i];
            return s + "\n";
        };
        f << "# shaderc_msl manifest v3 root=lib\n# columns\n";
        f << row({"split", "k1", "ms", "mesh", "cas/a.metallib", "1", "0:2,1:3", "0", "-", "-", "split64", "cas/s.metallib",
                  "cas/o.metallib", "3", "66", "128", "1", "0:2"});
        f << row({"msc", "k2", "ms", "mesh", "cas/b.metallib", "-", "-", "0", "-", "-", "mesh_as_compute", "-", "-", "-", "-", "-",
                  "-", "-", "cas/c.metallib", "cas/v.metallib", "0", "1", "-", "3:4", "30736,16,224,28688,16,128,128,36,152,128",
                  "0:16;1:32"});
        f << row({"xb", "k3", "cs", "kernel", "cas/x.metallib", "1", "-", "0", "-", "-", "-", "-", "-", "-", "-", "-", "-", "-",
                  "-", "-", "-", "-", "-", "-", "-", "-", "x_buf_device", "2151", "0"});
    }
    Catalog c;
    std::string err;
    bool ok = c.load(path, &err) && c.size() == 3;
    const ShaderEntry *s = c.find("split"), *m = c.find("msc"), *x = c.find("xb");
    ok = ok && s && s->split.present && s->split.chunks == 3 && s->split.vmax == 66 && s->split.pmax == 128 &&
         s->split.object_metallib == "cas/o.metallib" && s->split.push_buffer == 1 && s->split.tables.size() == 1 && !s->msc.present;
    ok = ok && m && m->msc.present && m->msc.slot_bytes == 30736 && m->msc.address_dword == 36 && m->msc.push_bytes == 152 &&
         m->msc.local_x == 128 && m->msc.params.size() == 2 && m->msc.vs_tables.size() == 1 && m->msc.cs_push == 0 && m->msc.vs_push == 1;
    ok = ok && x && x->cs_route == "x_buf_device" && x->x_buf_words == 2151 && x->x_buf_buffer == 0;
    printf("catalog v3 synthetic (split, mesh-as-compute, x_buf_device rows): %s%s%s\n", ok ? "OK" : "FAIL", err.empty() ? "" : " ", err.c_str());
    remove(path.c_str());
    return ok;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);
        if (!synthetic_ok()) return 1;
        if (!synthetic_v3_ok()) return 1;
        const char *man = argc > 1 ? argv[1] : getenv("SHADERC_MSL_MANIFEST");
        Catalog cat;
        std::string err;
        if (!man || !cat.load(man, &err)) {
            printf("catalog_test: no manifest (%s) (SKIP)\n", err.c_str());
            return 77;
        }
        printf("catalog: %zu shaders, root %s\n", cat.size(), cat.root().c_str());
        auto rows = data_rows(man);
        size_t resolved = 0, hash_rows = 0, hash_resolved = 0;
        std::map<std::string, size_t> stage_rows, stage_resolved;
        for (auto &r : rows) {
            const ShaderEntry *e = cat.find(r.name);
            bool ok = e && e->key == r.key && e->stage == r.stage;
            stage_rows[r.stage]++;
            if (ok) { resolved++; stage_resolved[r.stage]++; }
            if (!r.name.empty() && r.name[0] == '#') { hash_rows++; if (ok) hash_resolved++; }
        }
        bool count_ok = cat.size() == rows.size() && resolved == rows.size() && hash_resolved == hash_rows;
        printf("catalog: %zu data lines in manifest, %zu loaded, %zu resolve by name with matching key and stage; "
               "names starting with '#': %zu, %zu resolve\n", rows.size(), cat.size(), resolved, hash_rows, hash_resolved);
        for (auto &[st, n] : stage_rows) printf("catalog: stage %s %zu rows, %zu resolve\n", st.c_str(), n, stage_resolved[st]);
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        int built = 0, failed = 0, missing = 0, plan_bad = 0;
        for (auto &p : kPairs) {
            const ShaderEntry *vs = cat.find(p[1]), *ps = cat.find(p[2]);
            if (!vs || !ps) {
                printf("pair %s: not in manifest (vs %d ps %d)\n", p[0], vs != nullptr, ps != nullptr);
                missing++;
                continue;
            }
            NSError *e = nil;
            double t0 = [NSDate timeIntervalSinceReferenceDate];
            id<MTLLibrary> vl = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(cat.library_path(*vs).c_str())] error:&e];
            id<MTLLibrary> pl = vl ? [dev newLibraryWithURL:[NSURL fileURLWithPath:@(cat.library_path(*ps).c_str())] error:&e] : nil;
            if (!vl || !pl) {
                printf("pair %s: library load failed: %s\n", p[0], e.localizedDescription.UTF8String);
                failed++;
                continue;
            }
            MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
            d.vertexFunction = [vl newFunctionWithName:@"main0"];
            d.fragmentFunction = [pl newFunctionWithName:@"main0"];
            MTLVertexDescriptor *vd = [MTLVertexDescriptor new];
            NSUInteger off = 0;
            for (MTLVertexAttribute *a in d.vertexFunction.vertexAttributes) {
                vd.attributes[a.attributeIndex].format = fmt(a.attributeType);
                vd.attributes[a.attributeIndex].offset = off;
                vd.attributes[a.attributeIndex].bufferIndex = 30;
                off += 16;
            }
            vd.layouts[30].stride = off ? off : 16;
            d.vertexDescriptor = vd;
            d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            MTLRenderPipelineReflection *refl = nil;
            id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:d
                options:MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo reflection:&refl error:&e];
            double dt = [NSDate timeIntervalSinceReferenceDate] - t0;
            if (!pso) {
                printf("pair %s: pipeline failed: %s\n", p[0], e.localizedDescription.UTF8String);
                failed++;
                continue;
            }
            int mism = 0;
            std::regex set_re("spvDescriptorSet([0-9]+)");
            for (id<MTLBinding> b in refl.fragmentBindings) {
                std::cmatch m;
                std::string n = b.name.UTF8String;
                if (std::regex_match(n.c_str(), m, set_re)) {
                    int binding = std::stoi(m[1]) - 1;
                    bool found = false;
                    for (auto &t : ps->tables)
                        if (t.first == binding && t.second == (int)b.index) found = true;
                    if (!found) mism++;
                } else if (b.type == MTLBindingTypeBuffer && (int)b.index != ps->push_buffer && b.index < 30) {
                    mism++;
                }
            }
            if ((int)refl.fragmentBindings.count < (int)ps->tables.size()) mism++;
            if (mism) plan_bad++;
            std::string tables;
            for (auto &t : ps->tables) tables += " " + std::to_string(t.first) + "->buffer(" + std::to_string(t.second) + ")";
            printf("pair %s: pipeline OK in %.3f s; PS push buffer(%d), tables%s; reflection mismatches %d\n", p[0], dt,
                   ps->push_buffer, tables.c_str(), mism);
            built++;
        }
        printf("catalog_test: %d of %d first-scene pairs built from catalog metallibs, %d failed, %d missing, %d plan mismatches; "
               "manifest rows %s\n", built, kNumPairs, failed, missing, plan_bad, count_ok ? "all loaded" : "NOT all loaded");
        return (count_ok && built == kNumPairs && !plan_bad) ? 0 : 1;
    }
}
