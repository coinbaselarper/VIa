#include "pcache.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace gpu = gta6rt::gpu;
namespace pc = gta6rt::pcache;

static int failures = 0;
#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);   \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

static gpu::RenderPipelineDesc random_render(std::mt19937& r) {
    gpu::RenderPipelineDesc d;
    d.rt_count = r() % 9;
    for (auto& f : d.rt_formats) f = r() % 120;
    for (auto& b : d.blend) {
        b.enable = r() & 1;
        b.src = gpu::Blend(1 + r() % 19);
        b.dst = gpu::Blend(1 + r() % 19);
        b.op = gpu::BlendOp(1 + r() % 5);
        b.src_alpha = gpu::Blend(1 + r() % 19);
        b.dst_alpha = gpu::Blend(1 + r() % 19);
        b.op_alpha = gpu::BlendOp(1 + r() % 5);
        b.write_mask = std::uint8_t(r() & 0xf);
    }
    d.depth_format = r() % 60;
    d.samples = 1u << (r() % 4);
    d.raster.cull = gpu::CullMode(1 + r() % 3);
    d.raster.front_ccw = r() & 1;
    d.raster.wireframe = r() & 1;
    d.raster.depth_bias = int(r() % 2000) - 1000;
    d.raster.depth_bias_clamp = float(r() % 1000) / 7.0f;
    d.raster.slope_scaled_depth_bias = -float(r() % 1000) / 3.0f;
    d.raster.depth_clip = r() & 1;
    d.depth_stencil.depth_test = r() & 1;
    d.depth_stencil.depth_write = r() & 1;
    d.depth_stencil.depth_func = gpu::CompareFunc(r() % 9);
    d.depth_stencil.stencil = r() & 1;
    d.depth_stencil.stencil_read_mask = std::uint8_t(r());
    d.depth_stencil.stencil_write_mask = std::uint8_t(r());
    d.depth_stencil.front = {gpu::StencilOp(1 + r() % 8), gpu::StencilOp(1 + r() % 8), gpu::StencilOp(1 + r() % 8),
                             gpu::CompareFunc(r() % 9)};
    d.depth_stencil.back = {gpu::StencilOp(1 + r() % 8), gpu::StencilOp(1 + r() % 8), gpu::StencilOp(1 + r() % 8),
                            gpu::CompareFunc(r() % 9)};
    const std::uint32_t topologies[] = {1, 2, 3, 19, 32};
    d.topology = gpu::TopologyType(topologies[r() % 5]);
    d.rect = gpu::RectMode(r() % 3);
    d.d24 = gpu::D24Mode(r() % 2);
    for (unsigned i = 0, n = r() % 6; i < n; ++i)
        d.inputs.push_back({std::uint32_t(r() % 16), std::uint32_t(r() % 100), std::uint32_t(r() % 8),
                            std::uint32_t(r() % 256), bool(r() & 1), std::uint32_t(r() % 3)});
    return d;
}

int main(int argc, char** argv) {
    const std::filesystem::path dir = argc > 1 ? argv[1] : "pcache-test-tmp";
    std::filesystem::create_directories(dir);
    std::mt19937 r(7);
    const pc::ContentKeyFn ck = [](const std::string& n) { return n == "missing" ? std::string("-") : "abc" + n; };

    for (int i = 0; i < 2000; ++i) {
        const auto d = random_render(r);
        const std::string vs = i % 7 == 0 ? "%V weird=name@x~y,z" : "VS_" + std::to_string(i);
        const pc::Spec s = pc::spec_of(d, vs, i % 3 ? "PS_x" : "", ck);
        const std::string text = pc::encode(s);
        pc::Spec back;
        std::string why;
        CHECK(pc::decode(text, back, &why));
        if (!why.empty()) std::fprintf(stderr, "decode: %s\n%s\n", why.c_str(), text.c_str());
        CHECK(back.names[0] == vs);
        CHECK(pc::encode(back) == text);
        CHECK(text.find('\t') == std::string::npos && text.find('\n') == std::string::npos);
        CHECK(pc::key_of(text).size() == 16);
    }
    {
        gpu::MeshPipelineDesc m;
        m.mesh_threads = {128, 1, 1};
        m.rt_count = 2;
        m.rt_formats[0] = 28;
        m.rt_formats[1] = 10;
        m.precheck = false;
        const pc::Spec s = pc::spec_of(m, "MS_a", "PS_b", ck);
        pc::Spec back;
        CHECK(pc::decode(pc::encode(s), back));
        CHECK(pc::encode(back) == pc::encode(s));
        CHECK(back.mesh.mesh_threads[0] == 128 && !back.mesh.precheck);
        gpu::ComputePipelineDesc c;
        c.threads = {64, 2, 1};
        const pc::Spec cs = pc::spec_of(c, "library/lib_cs_00031", ck);
        CHECK(pc::decode(pc::encode(cs), back));
        CHECK(back.kind == pc::Kind::compute && back.compute.threads[1] == 2 && back.names[0] == "library/lib_cs_00031");
    }
    {
        gpu::RenderPipelineDesc a, b;
        b.label = "other";
        b.priority = gpu::Priority::background;
        b.mode = gpu::CompileMode::async;
        CHECK(pc::encode(pc::spec_of(a, "v", "p", ck)) == pc::encode(pc::spec_of(b, "v", "p", ck)));
        b.rt_formats[3] = 1;
        CHECK(pc::encode(pc::spec_of(a, "v", "p", ck)) != pc::encode(pc::spec_of(b, "v", "p", ck)));
        CHECK(pc::encode(pc::spec_of(a, "v", "p", ck)) != pc::encode(pc::spec_of(a, "v", "p", [](const std::string&) { return std::string("zzz"); })));
    }
    {
        pc::Spec bad;
        CHECK(!pc::decode("R vs=x", bad));
        CHECK(!pc::decode("X vs=x", bad));
        CHECK(!pc::decode("C cs=a@- th=1.1.1 zz=1", bad));
    }
    {
        const auto entry = [&](const std::string& name, pc::Phase p, double t) {
            gpu::ComputePipelineDesc c;
            pc::Entry e;
            e.text = pc::encode(pc::spec_of(c, name, ck));
            e.key = pc::key_of(e.text);
            e.phase = p;
            e.t = t;
            return e;
        };
        pc::Manifest run1, run2, all;
        run1.note(entry("w", pc::Phase::world, 200));
        run1.note(entry("a", pc::Phase::landing, 20));
        run1.note(entry("a", pc::Phase::boot, 1));
        run2.note(entry("w", pc::Phase::story_load, 90));
        run2.note(entry("b", pc::Phase::boot, 5));
        all.merge(run1, true);
        all.merge(run2, true);
        CHECK(all.size() == 3);
        const auto o = all.ordered();
        CHECK(o.size() == 3 && o[0].phase == pc::Phase::boot && o[1].phase == pc::Phase::landing);
        CHECK(o[2].phase == pc::Phase::story_load && o[2].t == 90 && o[2].runs == 2);
        const std::string path = (dir / "m.txt").string();
        std::string why;
        CHECK(all.save(path, &why));
        pc::Manifest back;
        CHECK(back.load(path, &why));
        CHECK(back.size() == 3);
        CHECK(back.ordered()[2].runs == 2);
        for (const auto& f : std::filesystem::directory_iterator(dir))
            CHECK(f.path().filename().string().find(".tmp") == std::string::npos);
        {
            std::ifstream in(path);
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::ofstream(path) << text.substr(0, text.size() - 7);
        }
        pc::Manifest torn;
        CHECK(!torn.load(path, &why));
        std::ofstream(path) << "# leonida boot manifest 1\nc abc\n";
        CHECK(!torn.load(path, &why));
    }
    {
        const std::string v = pc::archive_version(GTA6RT_TEST_PORT_ROOT, "/nonexistent");
        CHECK(v.find("pcache 1") != std::string::npos && v.find("catalog ?") != std::string::npos);
    }
    std::printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
