#include "test_util.h"

#include <gta6rt_metal/metal_backend.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>

using namespace gta6rt::gpu;

namespace {

constexpr std::uint32_t N = 64;

std::uint32_t fbits(float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

std::string big_mesh_source(bool clip) {
    std::ostringstream s;
    s << "#include <metal_stdlib>\nusing namespace metal;\n\nstruct BigVertex\n{\n    float4 gl_Position [[position]];\n";
    for (int i = 0; i < 30; ++i) s << "    float4 m_" << i << " [[user(locn" << i << ")]];\n";
    if (clip) s << "    float gl_ClipDistance [[clip_distance]] [1];\n    float gl_ClipDistance_0 [[user(clip0)]];\n";
    s << "};\n\nstruct Prim\n{\n    bool gl_CullPrimitiveEXT [[primitive_culled]];\n};\n\n"
         "using BigMesh = mesh<BigVertex, Prim, 128, 128, topology::triangle>;\n\n"
         "[[mesh]] void ms_big(uint lid [[thread_index_in_threadgroup]], BigMesh m)\n{\n"
         "    if (lid == 0u) m.set_primitive_count(128u);\n"
         "    if (lid < 128u)\n    {\n        BigVertex v = {};\n        v.gl_Position = float4(0.0, 0.0, 0.0, 1.0);\n";
    if (clip) s << "        v.gl_ClipDistance[0] = 1.0;\n        v.gl_ClipDistance_0 = 1.0;\n";
    for (int i = 0; i < 30; ++i) s << "        v.m_" << i << " = float4(float(lid + " << i << "u));\n";
    s << "        m.set_vertex(lid, v);\n        Prim p;\n        p.gl_CullPrimitiveEXT = true;\n        m.set_primitive(lid, p);\n"
         "        m.set_index(lid * 3u + 0u, lid);\n        m.set_index(lid * 3u + 1u, lid);\n        m.set_index(lid * 3u + 2u, lid);\n"
         "    }\n}\n";
    return s.str();
}

struct Env {
    Device* dev = nullptr;
    Queue* q = nullptr;
    std::unique_ptr<ShaderLibrary> lib;
    std::unique_ptr<RootLayout> root;
    std::unique_ptr<Texture> rt;
};

bool run(Env& e, CommandList& l) {
    if (l.close() != Status::ok) return false;
    CommandList* ls[] = {&l};
    return e.q->execute(ls) == Status::ok && e.q->wait_idle(20'000'000'000ull) == Status::ok;
}

int coloured(Env& e, std::array<std::uint8_t, 3> c) {
    std::vector<std::uint8_t> px;
    CHECK(test::read_texture(*e.dev, *e.q, *e.rt, 4, px), "readback");
    int n = 0;
    for (std::uint32_t i = 0; i < N * N; ++i)
        n += std::abs(px[i * 4] - c[0]) <= 1 && std::abs(px[i * 4 + 1] - c[1]) <= 1 && std::abs(px[i * 4 + 2] - c[2]) <= 1;
    return n;
}

std::unique_ptr<MeshPipeline> mesh_pipeline(Env& e, const char* object, const char* mesh) {
    MeshPipelineDesc d;
    if (object) {
        d.object = {e.lib.get(), object};
        d.object_threads = {8, 1, 1};
        d.payload_bytes = 32;
    }
    d.mesh = {e.lib.get(), mesh};
    d.ps = {e.lib.get(), "ps_main"};
    d.rt_formats[0] = format::r8g8b8a8_unorm;
    d.rt_count = 1;
    d.raster.cull = CullMode::none;
    d.root = e.root.get();
    auto p = e.dev->create_mesh_pipeline(d);
    CHECK(p->status() == Status::ok, "mesh pipeline %s: %s", mesh, p->error().c_str());
    return p;
}

void begin(Env& e, CommandList& l, MeshPipeline* p, std::array<float, 3> c, std::uint32_t per_object = 0) {
    TargetView v{e.rt.get(), 0, 0, 0};
    l.set_render_targets(std::span<const TargetView>(&v, 1), nullptr);
    l.clear_render_target(v, {0, 0, 0, 1});
    l.set_root_layout(e.root.get());
    l.set_mesh_pipeline(p);
    const std::uint32_t u[5] = {fbits(c[0]), fbits(c[1]), fbits(c[2]), 0, per_object};
    l.set_user_data(0, u);
}

void draws(Env& e) {
    auto ms = mesh_pipeline(e, nullptr, "ms_quad");
    auto os = mesh_pipeline(e, "os_main", "ms_payload");
    if (ms->status() != Status::ok || os->status() != Status::ok) return;
    std::printf("mesh pipelines: mesh-only threads %u,%u,%u; object+mesh object threads %u, mesh %u; estimate %llu B "
                "for the 4-vertex quad mesh\n",
                ms->mesh_threads()[0], ms->mesh_threads()[1], ms->mesh_threads()[2], os->object_threads()[0],
                os->mesh_threads()[0], (unsigned long long)ms->estimate().mesh_bytes);
    auto l = e.dev->create_command_list(QueueType::direct);
    begin(e, *l, ms.get(), {1, 0, 0});
    l->draw_mesh(8, 1, 1);
    CHECK(run(e, *l), "mesh draw");
    const int red = coloured(e, {255, 0, 0});
    std::printf("draw_mesh(8 groups), one 8x8 quad per group: %d red pixels (want 512)\n", red);
    CHECK(red == 512, "mesh-only draw");

    l = e.dev->create_command_list(QueueType::direct);
    begin(e, *l, os.get(), {0, 1, 0}, 4);
    l->draw_mesh(2, 1, 1);
    CHECK(run(e, *l), "object draw");
    const int green = coloured(e, {0, 255, 0});
    std::printf("object shader: 2 object groups x 4 mesh groups through the payload: %d green pixels (want 512)\n", green);
    CHECK(green == 512, "object + mesh draw");

    std::uint32_t recs[2][4] = {{4, 1, 1, 0}, {8, 1, 1, 0}};
    BufferDesc bd{sizeof(recs), MemoryKind::upload, usage::indirect, "mesh args"};
    auto args = e.dev->create_buffer(bd);
    std::memcpy(args->cpu_pointer(), recs, sizeof(recs));
    BufferDesc cd{16, MemoryKind::shared, usage::indirect, "mesh count"};
    auto count = e.dev->create_buffer(cd);
    *static_cast<std::uint32_t*>(count->cpu_pointer()) = 1;
    BufferDesc gd{64 * 1024, MemoryKind::shared, usage::unordered_access, "debug counters"};
    auto dbg = e.dev->create_buffer(gd);
    auto* dw = static_cast<std::uint32_t*>(dbg->cpu_pointer());
    std::memset(dw, 0, 64 * 1024);
    CHECK(gta6rt::metal::set_debug_buffer(e.dev, dbg.get()), "set_debug_buffer");
    for (int with_count = 1; with_count >= 0; --with_count) {
        l = e.dev->create_command_list(QueueType::direct);
        begin(e, *l, ms.get(), {0, 0, 1});
        IndirectDesc d;
        d.kind = IndirectKind::dispatch_mesh;
        d.stride = 16;
        d.max_count = 2;
        d.arguments = args.get();
        if (with_count) d.count_buffer = count.get();
        l->execute_indirect(d);
        const auto before = e.dev->stats();
        CHECK(run(e, *l), "indirect mesh");
        const auto after = e.dev->stats();
        const int blue = coloured(e, {0, 0, 255});
        std::printf("indirect mesh draws (4 and 8 groups), %s: %d blue pixels, hoisted pre-passes %llu\n",
                    with_count ? "count 1" : "no count buffer", blue,
                    (unsigned long long)(after.prepasses_hoisted - before.prepasses_hoisted));
        CHECK(blue == (with_count ? 256 : 512), "indirect mesh draw records");
        std::printf("debug counters: calls %u (with a count %u, counts summed %u), records %u (within the count %u, "
                    "with groups %u, past the limits %u), groups %u, first record %ux%ux%u\n",
                    dw[5], dw[6], dw[7], dw[0], dw[1], dw[2], dw[3], dw[4], dw[32], dw[33], dw[34]);
        CHECK(dw[5] == 1 && dw[6] == std::uint32_t(with_count) && dw[7] == std::uint32_t(with_count) && dw[0] == 2 &&
                  dw[1] == (with_count ? 1u : 2u) && dw[2] == dw[1] && dw[3] == 0 && dw[4] == (with_count ? 4u : 12u),
              "indirect pre-pass debug counters");
        std::memset(dw, 0, 4096);
    }
    gta6rt::metal::set_debug_buffer(e.dev, nullptr);
}

void refusals(Env& e) {
    for (bool clip : {false, true}) {
        std::string err;
        auto lib = e.dev->create_library_msl(big_mesh_source(clip), MslOptions{}, &err);
        CHECK(lib != nullptr, "big mesh library: %s", err.c_str());
        if (!lib) continue;
        for (bool precheck : {true, false}) {
            MeshPipelineDesc d;
            d.mesh = {lib.get(), "ms_big"};
            d.mesh_threads = {128, 1, 1};
            d.precheck = precheck;
            const auto t0 = std::chrono::steady_clock::now();
            auto p = e.dev->create_mesh_pipeline(d);
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("128 vertices x 30 float4%s, precheck %d: %s in %.4f s, limit %s value %llu limit %llu (%s); "
                        "estimate %llu B, %u vertex scalars: %s\n",
                        clip ? " + clip[1]" : "", precheck, status_name(p->status()), s,
                        limit_kind_name(p->limit().kind), (unsigned long long)p->limit().value,
                        (unsigned long long)p->limit().limit, p->limit().estimated ? "estimated" : "Metal",
                        (unsigned long long)p->estimate().mesh_bytes, p->estimate().vertex_scalars, p->error().c_str());
            const char* sv = std::getenv("MTL_SHADER_VALIDATION");
            const bool validation = sv && *sv && *sv != '0';
            if (validation && !precheck) {
                CHECK(p->status() != Status::ok, "oversized mesh refused under validation (%s)", p->error().c_str());
                continue;
            }
            CHECK(p->status() == Status::limit_exceeded && p->limit().kind == PipelineLimit::Kind::mesh_size &&
                      p->limit().value > 32768 && p->limit().estimated == precheck,
                  "oversized mesh refused with the mesh size (%s)", precheck ? "estimate" : "Metal");
            if (precheck) CHECK(p->limit().value == (clip ? 65536u : 63488u), "estimated mesh size");
            if (!precheck && !clip) CHECK(p->limit().value == 63488u, "Metal mesh size of the 30 float4 shape");
            if (!precheck && clip)
                CHECK(p->error().find("unique scalar count of 126") != std::string::npos,
                      "Metal also reports 126 unique scalars");
        }
    }
}

struct GameMesh {
    std::string stem, path, result, bytes, unique;
    std::uint32_t scalars = 0;
};

void corpus(Env& e, const char* manifest) {
    std::ifstream mf(manifest);
    if (!mf) {
        std::printf("corpus mesh shaders: no manifest, skipped\n");
        return;
    }
    RootLayoutDesc rd;
    for (std::uint32_t b = 0; b <= 28; ++b)
        rd.tables.push_back({b, b == 1 ? DescriptorKind::sampler : b == 2 ? DescriptorKind::buffer : DescriptorKind::texture});
    auto root = e.dev->create_root_layout(rd);
    int agree = 0, total = 0;
    const char* sv = std::getenv("MTL_SHADER_VALIDATION");
    const bool validation = sv && *sv && *sv != '0';
    if (validation) std::printf("shader validation on: Metal verdicts are reported, not checked (validation adds threadgroup storage)\n");
    for (std::string line; std::getline(mf, line);) {
        std::istringstream s(line);
        std::string kind;
        s >> kind;
        if (kind != "mesh") continue;
        GameMesh g;
        s >> g.stem >> g.path >> g.result >> g.bytes >> g.unique >> g.scalars;
        std::string err;
        auto lib = e.dev->create_library_msl(test::read_file(g.path), MslOptions{}, &err);
        CHECK(lib != nullptr, "%s: MSL compile: %s", g.stem.c_str(), err.c_str());
        if (!lib) continue;
        MeshPipelineDesc pre;
        pre.mesh = {lib.get(), "main0"};
        pre.root = root.get();
        auto pp = e.dev->create_mesh_pipeline(pre);
        MeshPipelineDesc full = pre;
        full.precheck = false;
        const auto t0 = std::chrono::steady_clock::now();
        auto pf = e.dev->create_mesh_pipeline(full);
        const double s2 = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const MeshEstimate& est = pp->estimate();
        const bool metal_ok = pf->status() == Status::ok;
        const bool pre_ok = !(pp->status() == Status::limit_exceeded && pp->limit().estimated);
        const bool s52_ok = g.result == "OK";
        ++total;
        agree += metal_ok == s52_ok && pre_ok == s52_ok;
        std::printf("corpus %s: S5.2 %s (%s B, unique %s, %u scalars); estimate %llu B, %u scalars; precheck %s; Metal "
                    "%s in %.3f s, threads %u: %s\n",
                    g.stem.c_str(), g.result.c_str(), g.bytes.c_str(), g.unique.c_str(), g.scalars,
                    (unsigned long long)est.mesh_bytes, est.vertex_scalars, status_name(pp->status()),
                    status_name(pf->status()), s2, pf->mesh_threads()[0],
                    metal_ok ? "" : pf->error().substr(0, 160).c_str());
        CHECK(est.parsed && est.vertex_scalars == g.scalars, "%s: estimator scalars = S5.2 counter", g.stem.c_str());
        CHECK(pre_ok == s52_ok, "%s: precheck verdict = S5.2 verdict", g.stem.c_str());
        if (validation) continue;
        CHECK(metal_ok == s52_ok, "%s: Metal verdict = S5.2 verdict", g.stem.c_str());
        if (!metal_ok) {
            CHECK(pf->status() == Status::limit_exceeded && g.bytes != "-" && pf->limit().value == std::stoull(g.bytes),
                  "%s: Metal refusal parsed with the S5.2 mesh size", g.stem.c_str());
            if (g.unique != "-")
                CHECK(pf->error().find("unique scalar count of " + g.unique) != std::string::npos,
                      "%s: unique scalar count %s reported", g.stem.c_str(), g.unique.c_str());
        }
        if (metal_ok) CHECK(pf->reflection().mismatches.empty(), "%s: no binding mismatch", g.stem.c_str());
    }
    std::printf("corpus mesh shaders: %d of %d agree with S5.2 (precheck and Metal)\n", agree, total);
}

}  // namespace

int main(int argc, char** argv) {
    auto dev = gta6rt::metal::create_device();
    if (!dev) return 1;
    test::print_caps(dev->caps());
    std::printf("mesh limits used: %u B, %u unique scalars\n", dev->caps().max_mesh_bytes, dev->caps().max_mesh_scalars);
    auto queue = dev->create_queue(QueueType::direct, "mesh");
    Env e;
    e.dev = dev.get();
    e.q = queue.get();
    std::string err;
    e.lib = dev->create_library_msl(test::read_file(test::shader_path("mesh.metal")), MslOptions{}, &err);
    CHECK(e.lib != nullptr, "library: %s", err.c_str());
    if (!e.lib) return test::finish("metal_test_mesh");
    e.root = dev->create_root_layout(RootLayoutDesc{});
    TextureDesc td;
    td.format = format::r8g8b8a8_unorm;
    td.width = td.height = N;
    td.usage = usage::render_target | usage::shader_read;
    e.rt = dev->create_texture(td);
    draws(e);
    refusals(e);
    if (argc > 1) corpus(e, argv[1]);
    test::print_stats(dev->stats());
    return test::finish("metal_test_mesh");
}
