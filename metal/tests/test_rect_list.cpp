#include "test_util.h"

#include "gta6rt_metal/rect_list.h"

#include <cmath>
#include <cstring>

using namespace gta6rt::gpu;
namespace rect = gta6rt::rect;

namespace {

const std::uint32_t N = 64;

struct V {
    float x, y, z, w, u, v;
};

void cpu_rules() {
    using XY = std::array<std::array<float, 2>, 3>;
    const std::array<float, 2> tl{0, 0}, bl{0, 1}, br{1, 1}, tr{1, 0};
    CHECK(rect::right_angle(XY{tl, bl, br}) == 1, "corner at v1");
    CHECK(rect::right_angle(XY{tl, bl, tr}) == 0, "corner at v0");
    CHECK(rect::right_angle(XY{bl, tr, tl}) == 2, "corner at v2");
    CHECK(rect::right_angle(XY{{{0, 0}, {3, 1}, {1, 0}}}) == 2, "obtuse at v2");
    CHECK(rect::right_angle(XY{tl, tl, tl}) == 1, "a point");
    CHECK(rect::right_angle(XY{{tl, {NAN, 0}, br}}) == 1, "NaN");
    CHECK(rect::right_angle(XY{{{-1, 1}, {-1, -1}, {1, -1}}}) == 1, "VS_Blit_6e5520d1 corner (second vertex)");

    const V src[3] = {{-1, 1, 0, 1, 0, 0}, {-1, -1, 0, 1, 0, 1}, {1, -1, 0, 1, 1, 1}};
    const rect::CpuElement el[] = {{0, format::r32g32b32a32_float}, {16, format::r32g32_float}};
    rect::CpuExpansion ex;
    CHECK(rect::expand_rect_vertices({reinterpret_cast<const std::uint8_t*>(src), sizeof(src)}, sizeof(V), el, 1, {}, ex),
          "expand");
    const V* out = reinterpret_cast<const V*>(ex.vertices.data());
    CHECK(ex.corners.size() == 1 && ex.corners[0] == 1, "corner");
    CHECK(out[3].x == 1 && out[3].y == 1 && out[3].u == 1 && out[3].v == 0, "fourth vertex (%g %g %g %g)", out[3].x,
          out[3].y, out[3].u, out[3].v);
    const std::uint32_t want[6] = {0, 1, 2, 0, 2, 3};
    CHECK(ex.indices.size() == 6 && std::equal(want, want + 6, ex.indices.begin()), "indices");
}

struct Ctx {
    Device* dev;
    Queue* q;
    Texture* target;
    DescriptorHeap* images;
    DescriptorHeap* samplers;
    RootLayout* root;
};

enum class Pass { implicit, explicit_clear, depth_discard };

std::vector<std::uint8_t> draw(Ctx& c, RenderPipeline* p, std::span<const V> verts, std::span<const std::uint16_t> idx,
                               std::uint32_t topo, std::int32_t base_vertex = 0, Pass pass = Pass::implicit,
                               Texture* depth = nullptr) {
    BufferDesc vbd{verts.size_bytes() + 256, MemoryKind::upload, usage::vertex, "vb"};
    auto vb = c.dev->create_buffer(vbd);
    std::memcpy(static_cast<std::uint8_t*>(vb->cpu_pointer()) + 64, verts.data(), verts.size_bytes());
    std::unique_ptr<Buffer> ib;
    if (!idx.empty()) {
        BufferDesc ibd{idx.size_bytes() + 64, MemoryKind::upload, usage::index, "ib"};
        ib = c.dev->create_buffer(ibd);
        std::memcpy(ib->cpu_pointer(), idx.data(), idx.size_bytes());
    }
    auto l = c.dev->create_command_list(QueueType::direct);
    TargetView rt{c.target, 0, 0, 0};
    TargetView dv{depth, 0, 0, 0};
    if (pass == Pass::explicit_clear) {
        RenderPassDesc rp;
        rp.targets[0].view = rt;
        rp.targets[0].load = LoadOp::clear;
        rp.targets[0].store = StoreOp::preserve;
        rp.target_count = 1;
        l->begin_render_pass(rp);
    } else {
        l->set_render_targets(std::span<const TargetView>(&rt, 1), depth ? &dv : nullptr);
        l->clear_render_target(rt, {0, 0, 0, 0});
        if (depth) l->clear_depth_stencil(dv, true, 1.0f, false, 0);
    }
    l->set_pipeline(p);
    l->set_root_layout(c.root);
    const float one = 1.0f;
    std::uint32_t user[5] = {3, 2, 0, 0, 0};
    std::memcpy(&user[4], &one, 4);
    l->set_user_data(0, user);
    l->set_descriptor_table(0, c.images);
    l->set_descriptor_table(1, c.samplers);
    VertexBufferView v{vb->gpu_va() + 64, std::uint32_t(verts.size_bytes()), sizeof(V)};
    l->set_vertex_buffers(0, std::span<const VertexBufferView>(&v, 1));
    l->set_topology(topo);
    if (ib) {
        l->set_index_buffer({ib->gpu_va(), std::uint32_t(idx.size_bytes()), format::r16_uint});
        l->draw_indexed(std::uint32_t(idx.size()), 1, 0, base_vertex, 0);
    } else {
        l->draw(std::uint32_t(verts.size()), 1, 0, 0);
    }
    if (pass == Pass::explicit_clear) l->end_render_pass();
    if (pass == Pass::depth_discard) l->discard(depth);
    CHECK(l->close() == Status::ok, "close");
    CommandList* lists[] = {l.get()};
    CHECK(c.q->execute(lists) == Status::ok, "execute");
    std::vector<std::uint8_t> px;
    CHECK(test::read_texture(*c.dev, *c.q, *c.target, 4, px), "readback");
    return px;
}

int differing(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    if (a.size() != b.size()) return -1;
    int n = 0;
    for (std::size_t i = 0; i < a.size(); i += 4) n += std::memcmp(&a[i], &b[i], 4) != 0;
    return n;
}

int covered(const std::vector<std::uint8_t>& a) {
    int n = 0;
    for (std::size_t i = 0; i < a.size(); i += 4) n += a[i + 3] != 0;
    return n;
}

std::vector<V> triangles_for(const V* r, int corner) {
    const int a = (corner + 2) % 3, b = (corner + 1) % 3;
    V v4 = r[a];
    v4.x = r[a].x + r[b].x - r[corner].x;
    v4.y = r[a].y + r[b].y - r[corner].y;
    v4.z = r[a].z + r[b].z - r[corner].z;
    v4.w = r[a].w + r[b].w - r[corner].w;
    v4.u = r[a].u + r[b].u - r[corner].u;
    v4.v = r[a].v + r[b].v - r[corner].v;
    const V q[4] = {r[0], r[1], r[2], v4};
    std::vector<V> out;
    for (std::uint8_t k : rect::triangle_corners(corner)) out.push_back(q[k]);
    return out;
}

V nl(V v) {
    v.u = v.u * v.v;
    return v;
}

}  // namespace

int main() {
    cpu_rules();
    auto dev = gta6rt::metal::create_device();
    if (!dev) return 1;
    auto q = dev->create_queue(QueueType::direct, "rect");

    TextureDesc td;
    td.format = format::r8g8b8a8_unorm;
    td.width = N;
    td.height = N;
    td.usage = usage::shader_read;
    td.memory = MemoryKind::shared;
    auto src = dev->create_texture(td);
    std::vector<std::uint8_t> texels(N * N * 4);
    for (std::uint32_t y = 0; y < N; ++y)
        for (std::uint32_t x = 0; x < N; ++x) {
            std::uint8_t* t = &texels[(y * N + x) * 4];
            t[0] = std::uint8_t(x * 4);
            t[1] = std::uint8_t(y * 4);
            t[2] = std::uint8_t(((x ^ y) * 4) & 0xff);
            t[3] = 255;
        }
    CHECK(src->write_region(0, 0, 0, 0, N, N, texels.data(), N * 4) == Status::ok, "source texels");
    td.memory = MemoryKind::device_local;
    td.usage = usage::render_target | usage::shader_read;
    auto target = dev->create_texture(td);
    SamplerDesc sd;
    sd.min = sd.mag = sd.mip = Filter::point;
    sd.u = sd.v = sd.w = AddressMode::clamp;
    auto smp = dev->create_sampler(sd);
    auto images = dev->create_descriptor_heap({DescriptorKind::texture, 8, "images"});
    auto samplers = dev->create_descriptor_heap({DescriptorKind::sampler, 8, "samplers"});
    images->write_texture(3, src.get());
    samplers->write_sampler(2, smp.get());
    RootLayoutDesc rd;
    rd.tables = {{0, DescriptorKind::texture}, {1, DescriptorKind::sampler}};
    auto root = dev->create_root_layout(rd);
    std::string err;
    auto lib = dev->create_library_msl(test::read_file(test::shader_path("blit.metal")), MslOptions{}, &err);
    CHECK(lib != nullptr, "blit library: %s", err.c_str());
    if (!lib) return test::finish("metal_test_rect_list");

    auto make = [&](const char* vs, TopologyType topo, RectMode mode, std::uint32_t depth_format = format::unknown) {
        RenderPipelineDesc pd;
        pd.vs = {lib.get(), vs};
        pd.ps = {lib.get(), "ps_main"};
        pd.inputs = {{0, format::r32g32b32a32_float, 0, 0}, {1, format::r32g32_float, 0, 16}};
        pd.rt_formats[0] = format::r8g8b8a8_unorm;
        pd.rt_count = 1;
        pd.raster.cull = CullMode::none;
        pd.root = root.get();
        pd.topology = topo;
        pd.rect = mode;
        pd.depth_format = depth_format;
        auto p = dev->create_render_pipeline(pd);
        CHECK(p->wait() == Status::ok, "pipeline %s: %s", vs, p->error().c_str());
        return p;
    };
    auto tri = make("main0", TopologyType::triangle, RectMode::none);
    auto rect_gpu = make("main0", TopologyType::rectangle, RectMode::vertex_expand);
    auto rect_cpu = make("main0", TopologyType::rectangle, RectMode::cpu_rewrite);
    auto rect_gpu_nl = make("main0_nl", TopologyType::rectangle, RectMode::vertex_expand);
    auto rect_cpu_nl = make("main0_nl", TopologyType::rectangle, RectMode::cpu_rewrite);
    CHECK(rect_gpu->rect_mode() == RectMode::vertex_expand, "wrapper accepted (%s)",
          static_cast<const char*>(rect_gpu->rect_mode() == RectMode::vertex_expand ? "" : "fell back"));
    Ctx c{dev.get(), q.get(), target.get(), images.get(), samplers.get(), root.get()};

    const V blit[3] = {{-1, 1, 0, 1, 0, 0}, {-1, -1, 0, 1, 0, 1}, {1, -1, 0, 1, 1, 1}};
    const auto ref = draw(c, tri.get(), triangles_for(blit, 1), {}, topology::triangle_list);
    CHECK(differing(ref, texels) == 0, "reference blit equals the source (%d texels differ)", differing(ref, texels));
    const auto g1 = draw(c, rect_gpu.get(), blit, {}, topology::rect_list);
    const auto c1 = draw(c, rect_cpu.get(), blit, {}, topology::rect_list);
    std::printf("full-screen blit as one rectangle (corner at the second vertex): vertex_expand %d, cpu_rewrite %d "
                "pixels differ from the reference; covered %d / %d\n",
                differing(g1, ref), differing(c1, ref), covered(g1), int(N * N));
    CHECK(differing(g1, ref) == 0, "vertex_expand blit");
    CHECK(differing(c1, ref) == 0, "cpu_rewrite blit");

    const V two[6] = {{-1, -1, 0, 1, 0, 1}, {0, -1, 0, 1, 0.5f, 1}, {-1, 1, 0, 1, 0, 0},
                      {0, 1, 0, 1, 0.5f, 0},  {1, -1, 0, 1, 1, 1},   {1, 1, 0, 1, 1, 0}};
    const V padded[8] = {two[0], two[0], two[0], two[1], two[2], two[3], two[4], two[5]};
    const std::uint16_t idx[6] = {0, 1, 2, 3, 4, 5};
    std::vector<V> ref2 = triangles_for(&two[0], 0);
    const auto t2 = triangles_for(&two[3], 2);
    ref2.insert(ref2.end(), t2.begin(), t2.end());
    const auto r2 = draw(c, tri.get(), ref2, {}, topology::triangle_list);
    const auto g2 = draw(c, rect_gpu.get(), padded, idx, topology::rect_list, 2);
    const auto c2 = draw(c, rect_cpu.get(), padded, idx, topology::rect_list, 2);
    std::printf("two indexed rectangles (corners at the first and third vertex, base vertex 2): vertex_expand %d, "
                "cpu_rewrite %d pixels differ; reference covers %d / %d\n",
                differing(g2, r2), differing(c2, r2), covered(r2), int(N * N));
    CHECK(differing(r2, texels) == 0, "two-rectangle reference equals the source");
    CHECK(differing(g2, r2) == 0, "vertex_expand indexed");
    CHECK(differing(c2, r2) == 0, "cpu_rewrite indexed");

    const V small[3] = {{-0.5f, 0.5f, 0, 1, 0.25f, 0.25f}, {-0.5f, -0.5f, 0, 1, 0.25f, 0.75f}, {0.5f, -0.5f, 0, 1, 0.75f, 0.75f}};
    const V post[3] = {nl(small[0]), nl(small[1]), nl(small[2])};
    const auto r3 = draw(c, tri.get(), triangles_for(post, 1), {}, topology::triangle_list);
    const auto g3 = draw(c, rect_gpu_nl.get(), small, {}, topology::rect_list);
    const auto c3 = draw(c, rect_cpu_nl.get(), small, {}, topology::rect_list);
    std::printf("vertex shader not linear in its inputs: vertex_expand %d, cpu_rewrite %d pixels differ from the "
                "hardware rule (fourth corner from vertex-stage outputs); %d covered\n",
                differing(g3, r3), differing(c3, r3), covered(r3));
    CHECK(differing(g3, r3) == 0, "vertex_expand is exact after the vertex stage");
    CHECK(differing(c3, r3) > 0, "cpu_rewrite differs for a non-linear vertex shader (expected)");

    const auto ex = draw(c, tri.get(), triangles_for(blit, 1), {}, topology::triangle_list, 0, Pass::explicit_clear);
    CHECK(differing(ex, ref) == 0, "explicit render pass (load clear, store preserve) equals the implicit one");
    auto tri_d = make("main0", TopologyType::triangle, RectMode::none, format::d32_float);
    TextureDesc dd;
    dd.format = format::d32_float;
    dd.width = N;
    dd.height = N;
    dd.usage = usage::depth_stencil;
    auto depth = dev->create_texture(dd);
    const auto s0 = dev->stats();
    const auto dz = draw(c, tri_d.get(), triangles_for(blit, 1), {}, topology::triangle_list, 0, Pass::depth_discard,
                         depth.get());
    const auto s1 = dev->stats();
    std::printf("explicit pass: %d pixels differ; depth discarded after the draw: %llu store(s) skipped, %llu clear(s) "
                "folded, %d colour pixels differ\n",
                differing(ex, ref), (unsigned long long)(s1.stores_discarded - s0.stores_discarded),
                (unsigned long long)(s1.clears_folded - s0.clears_folded), differing(dz, ref));
    CHECK(s1.stores_discarded - s0.stores_discarded == 1, "discarded depth is not stored");
    CHECK(s1.clears_folded - s0.clears_folded == 2, "colour and depth clears folded into the pass");
    CHECK(differing(dz, ref) == 0, "colour still stored");

    const auto st = dev->stats();
    std::printf("rect draws expanded: %llu\n", (unsigned long long)st.rect_draws_expanded);
    test::print_stats(st);
    return test::finish("metal_test_rect_list");
}
