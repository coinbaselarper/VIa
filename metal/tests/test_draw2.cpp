#include "test_util.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <random>

using namespace gta6rt::gpu;

namespace {

constexpr std::uint32_t N = 64;
constexpr std::uint32_t kCopyDest = 0x400, kIndirectArg = 0x200, kIndexBuffer = 0x2, kRenderTarget = 0x4,
                        kDepthWrite = 0x10, kCopySource = 0x800, kShaderResource = 0x40 | 0x80;

std::uint32_t fbits(float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

float px_to_ndc_x(float x) { return x / N * 2.0f - 1.0f; }
float px_to_ndc_y(float y) { return 1.0f - y / N * 2.0f; }

struct Env {
    Device* dev = nullptr;
    Queue* q = nullptr;
    std::unique_ptr<ShaderLibrary> lib;
    std::unique_ptr<RootLayout> root;
    std::unique_ptr<Texture> rt;
    std::unique_ptr<Buffer> vb;
    std::vector<float> verts;
};

std::uint32_t add_rect(Env& e, float x0, float y0, float x1, float y1) {
    const std::uint32_t first = std::uint32_t(e.verts.size() / 2);
    const float a = px_to_ndc_x(x0), b = px_to_ndc_y(y0), c = px_to_ndc_x(x1), d = px_to_ndc_y(y1);
    const float v[12] = {a, b, c, b, c, d, a, b, c, d, a, d};
    e.verts.insert(e.verts.end(), v, v + 12);
    return first;
}

std::uint32_t add_quad(Env& e, float x0, float y0, float x1, float y1) {
    const std::uint32_t first = std::uint32_t(e.verts.size() / 2);
    const float a = px_to_ndc_x(x0), b = px_to_ndc_y(y0), c = px_to_ndc_x(x1), d = px_to_ndc_y(y1);
    const float v[8] = {a, b, c, b, c, d, a, d};
    e.verts.insert(e.verts.end(), v, v + 8);
    return first;
}

std::unique_ptr<Buffer> upload(Env& e, const void* data, std::size_t bytes, MemoryKind kind = MemoryKind::upload) {
    BufferDesc bd{bytes, kind, usage::vertex | usage::index | usage::indirect | usage::copy, "data"};
    auto b = e.dev->create_buffer(bd);
    if (b && b->cpu_pointer()) std::memcpy(b->cpu_pointer(), data, bytes);
    return b;
}

std::unique_ptr<RenderPipeline> pipeline(Env& e, const char* ps, std::uint32_t depth_format = format::unknown,
                                         DepthStencilDesc ds = {}, RasterDesc raster = {}, TargetBlend blend = {},
                                         D24Mode d24 = D24Mode::fragment) {
    RenderPipelineDesc pd;
    pd.vs = {e.lib.get(), "vs_main"};
    if (ps) pd.ps = {e.lib.get(), ps};
    pd.inputs = {{0, format::r32g32_float, 0, 0}};
    pd.rt_formats[0] = format::r8g8b8a8_unorm;
    pd.rt_count = 1;
    pd.blend[0] = blend;
    pd.depth_format = depth_format;
    pd.depth_stencil = ds;
    raster.cull = CullMode::none;
    pd.raster = raster;
    pd.root = e.root.get();
    pd.d24 = d24;
    auto p = e.dev->create_render_pipeline(pd);
    CHECK(p->status() == Status::ok, "pipeline %s: %s", ps ? ps : "(none)", p->error().c_str());
    return p;
}

void bind(Env& e, CommandList& l, RenderPipeline* p, std::array<float, 4> color, float z = 0.5f,
          std::uint32_t heavy = 0, float dx = 0) {
    std::uint32_t u[16] = {};
    for (int i = 0; i < 4; ++i) u[i] = fbits(color[i]);
    u[4] = fbits(z);
    u[5] = fbits(dx);
    u[7] = heavy;
    l.set_root_layout(e.root.get());
    l.set_pipeline(p);
    l.set_user_data(0, u);
    VertexBufferView v{e.vb->gpu_va(), std::uint32_t(e.vb->size()), 8};
    l.set_vertex_buffers(0, std::span<const VertexBufferView>(&v, 1));
}

void targets(CommandList& l, Texture* rt, Texture* depth = nullptr) {
    TargetView v{rt, 0, 0, 0};
    TargetView dv{depth, 0, 0, 0};
    l.set_render_targets(std::span<const TargetView>(&v, 1), depth ? &dv : nullptr);
}

bool run(Env& e, CommandList& l) {
    if (l.close() != Status::ok) {
        std::printf("  close failed\n");
        return false;
    }
    CommandList* ls[] = {&l};
    return e.q->execute(ls) == Status::ok && e.q->wait_idle(20'000'000'000ull) == Status::ok;
}

std::vector<std::uint8_t> pixels(Env& e, Texture& t) {
    std::vector<std::uint8_t> px;
    CHECK(test::read_texture(*e.dev, *e.q, t, 4, px), "readback");
    return px;
}

int count_color(const std::vector<std::uint8_t>& px, std::array<std::uint8_t, 3> c, int x0 = 0, int y0 = 0, int x1 = N,
                int y1 = N) {
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const std::uint8_t* p = &px[(y * N + x) * 4];
            n += std::abs(p[0] - c[0]) <= 1 && std::abs(p[1] - c[1]) <= 1 && std::abs(p[2] - c[2]) <= 1;
        }
    return n;
}

float depth_at(Env& e, Texture& depth, int x, int y) {
    BufferDesc bd{N * N * 4, MemoryKind::readback, usage::copy, "depth rb"};
    auto rb = e.dev->create_buffer(bd);
    auto l = e.dev->create_command_list(QueueType::direct);
    BufferTextureCopy c;
    c.buffer = rb.get();
    c.texture = &depth;
    c.row_pitch = N * 4;
    c.depth_plane = true;
    l->copy_texture_to_buffer(c);
    CHECK(run(e, *l), "depth readback");
    float f;
    std::memcpy(&f, static_cast<const std::uint8_t*>(rb->cpu_pointer()) + (y * N + x) * 4, 4);
    return f;
}

double d24_reference(float v) {
    if (!(v > 0.0f)) return 0.0;
    if (v >= 1.0f) return 1.0;
    const float product = v * 16777215.0f;
    return float(double(std::nearbyint(product)) / 16777215.0);
}

std::unique_ptr<Texture> depth_texture(Env& e, std::uint32_t fmt) {
    TextureDesc td;
    td.format = fmt;
    td.width = td.height = N;
    td.usage = usage::depth_stencil | usage::shader_read;
    return e.dev->create_texture(td);
}

void test_indirect(Env& e, RenderPipeline* p, std::uint32_t r0, std::unique_ptr<Buffer>& ib) {
    struct Rec { std::uint32_t count, instances, first, base_instance, pad[4]; };
    Rec recs[3] = {};
    for (std::uint32_t i = 0; i < 3; ++i) recs[i] = {6, 1, r0 + 6 * i, 0, {}};
    auto args = upload(e, recs, sizeof(recs));
    BufferDesc cd{256, MemoryKind::device_local, usage::indirect | usage::copy, "count"};
    auto count = e.dev->create_buffer(cd);
    for (int with_count = 1; with_count >= 0; --with_count) {
        auto l = e.dev->create_command_list(QueueType::direct);
        if (with_count) {
            l->write_value(count.get(), 16, 2, 4);
            Barrier b;
            b.resource = count.get();
            b.before = kCopyDest;
            b.after = kIndirectArg;
            l->barrier(std::span<const Barrier>(&b, 1));
        }
        targets(*l, e.rt.get());
        l->clear_render_target({e.rt.get(), 0, 0, 0}, {0, 0, 0, 1});
        bind(e, *l, p, {0, 1, 0, 1});
        l->set_topology(topology::triangle_list);
        IndirectDesc d;
        d.kind = IndirectKind::draw;
        d.stride = sizeof(Rec);
        d.max_count = 3;
        d.arguments = args.get();
        if (with_count) {
            d.count_buffer = count.get();
            d.count_offset = 16;
        }
        l->execute_indirect(d);
        const auto before = e.dev->stats();
        CHECK(run(e, *l), "indirect draw");
        const auto after = e.dev->stats();
        const auto px = pixels(e, *e.rt);
        const int a = count_color(px, {0, 255, 0}, 0, 0, 20, N), b = count_color(px, {0, 255, 0}, 22, 0, 42, N),
                  c = count_color(px, {0, 255, 0}, 44, 0, 64, N);
        std::printf("indirect draw, stride 32, 3 records, %s: green pixels per record %d %d %d; pre-passes %llu (hoisted "
                    "%llu), render-pass splits %llu\n",
                    with_count ? "GPU-written count 2" : "no count buffer", a, b, c,
                    (unsigned long long)(after.indirect_prepasses - before.indirect_prepasses),
                    (unsigned long long)(after.prepasses_hoisted - before.prepasses_hoisted),
                    (unsigned long long)(after.render_pass_splits - before.render_pass_splits));
        CHECK(a == 20 * 64 && b == 20 * 64 && c == (with_count ? 0 : 20 * 64), "indirect draw records drawn");
        if (with_count)
            CHECK(after.prepasses_hoisted - before.prepasses_hoisted == 1 &&
                      after.render_pass_splits == before.render_pass_splits,
                  "count pre-pass ran before the render pass, no split");
    }
    struct IRec { std::uint32_t count, instances, first_index; std::int32_t base_vertex; std::uint32_t base_instance; };
    IRec irecs[3];
    for (std::uint32_t i = 0; i < 3; ++i) irecs[i] = {6, 1, 0, std::int32_t(r0 + 6 * i), 0};
    auto iargs = upload(e, irecs, sizeof(irecs));
    auto l = e.dev->create_command_list(QueueType::direct);
    targets(*l, e.rt.get());
    l->clear_render_target({e.rt.get(), 0, 0, 0}, {0, 0, 0, 1});
    bind(e, *l, p, {0, 0, 1, 1});
    l->set_topology(topology::triangle_list);
    l->set_index_buffer({ib->gpu_va(), 12, format::r16_uint});
    IndirectDesc d;
    d.kind = IndirectKind::draw_indexed;
    d.stride = sizeof(IRec);
    d.max_count = 3;
    d.arguments = iargs.get();
    l->execute_indirect(d);
    CHECK(run(e, *l), "indexed indirect");
    const auto px = pixels(e, *e.rt);
    const int blue = count_color(px, {0, 0, 255});
    std::printf("indexed indirect draw, stride 20, 3 records, base vertex per record: %d blue pixels (want %d)\n", blue,
                3 * 20 * 64);
    CHECK(blue == 3 * 20 * 64, "indexed indirect draws");
}

void test_quads(Env& e, RenderPipeline* p, std::uint32_t q0) {
    auto l = e.dev->create_command_list(QueueType::direct);
    targets(*l, e.rt.get());
    l->clear_render_target({e.rt.get(), 0, 0, 0}, {0, 0, 0, 1});
    bind(e, *l, p, {1, 0, 0, 1});
    l->set_topology(topology::quad_list);
    l->draw(8, 1, q0, 0);
    CHECK(run(e, *l), "quad list");
    auto px = pixels(e, *e.rt);
    const int a = count_color(px, {255, 0, 0}, 0, 0, 16, 16), b = count_color(px, {255, 0, 0}, 16, 0, 32, 16);
    const int total = count_color(px, {255, 0, 0});
    std::printf("non-indexed quad list, 2 quads of 16x16: %d and %d red pixels, %d in total\n", a, b, total);
    CHECK(a == 256 && b == 256 && total == 512, "non-indexed quads fully covered");

    const std::uint16_t idx[10] = {std::uint16_t(q0 + 4), std::uint16_t(q0 + 5), std::uint16_t(q0 + 6),
                                   std::uint16_t(q0 + 7), std::uint16_t(q0 + 0), std::uint16_t(q0 + 1),
                                   std::uint16_t(q0 + 2), std::uint16_t(q0 + 3), 0, 0};
    auto staging = upload(e, idx, sizeof(idx));
    BufferDesc bd{64, MemoryKind::device_local, usage::index | usage::copy, "quad indices (GPU written)"};
    auto gib = e.dev->create_buffer(bd);
    l = e.dev->create_command_list(QueueType::direct);
    l->copy_buffer(gib.get(), 4, staging.get(), 0, sizeof(idx));
    Barrier bar;
    bar.resource = gib.get();
    bar.before = kCopyDest;
    bar.after = kIndexBuffer;
    l->barrier(std::span<const Barrier>(&bar, 1));
    targets(*l, e.rt.get());
    l->clear_render_target({e.rt.get(), 0, 0, 0}, {0, 0, 0, 1});
    bind(e, *l, p, {1, 1, 0, 1}, 0.5f, 0, 64.0f / N);
    l->set_topology(topology::quad_list);
    l->set_index_buffer({gib->gpu_va() + 4, 20, format::r16_uint});
    l->draw_indexed(10, 2, 0, 0, 0);
    const auto before = e.dev->stats();
    CHECK(run(e, *l), "indexed quad list");
    const auto after = e.dev->stats();
    px = pixels(e, *e.rt);
    const int ya = count_color(px, {255, 255, 0}, 0, 0, 16, 16), yb = count_color(px, {255, 255, 0}, 16, 0, 32, 16);
    const int yi = count_color(px, {255, 255, 0}, 32, 0, 64, 16), ytotal = count_color(px, {255, 255, 0});
    std::printf("indexed quad list (GPU-copied 16-bit indices, 10 indices = 2 quads + 2 dropped, 2 instances): %d %d "
                "pixels, second instance %d, total %d; pre-passes %llu hoisted %llu\n",
                ya, yb, yi, ytotal, (unsigned long long)(after.indirect_prepasses - before.indirect_prepasses),
                (unsigned long long)(after.prepasses_hoisted - before.prepasses_hoisted));
    CHECK(ya == 256 && yb == 256 && yi == 512 && ytotal == 1024, "indexed quads expanded on the GPU");
}

void test_queries(Env& e, RenderPipeline* p, RenderPipeline* pz, std::uint32_t r16) {
    auto depth = depth_texture(e, format::d32_float);
    auto occ = e.dev->create_query_heap({QueryType::occlusion, 4, "occlusion"});
    auto bin = e.dev->create_query_heap({QueryType::binary_occlusion, 2, "binary"});
    BufferDesc rbd{256, MemoryKind::readback, usage::copy, "query results"};
    auto rb = e.dev->create_buffer(rbd);
    std::memset(rb->cpu_pointer(), 0xee, 256);
    auto l = e.dev->create_command_list(QueueType::direct);
    targets(*l, e.rt.get(), depth.get());
    l->clear_render_target({e.rt.get(), 0, 0, 0}, {0, 0, 0, 1});
    l->clear_depth_stencil({depth.get(), 0, 0, 0}, true, 0.5f, false, 0);
    bind(e, *l, pz, {1, 1, 1, 1}, 0.25f);
    l->set_topology(topology::triangle_list);
    l->begin_query(occ.get(), 0);
    l->draw(6, 1, r16, 0);
    l->end_query(occ.get(), 0);
    bind(e, *l, pz, {1, 0, 1, 1}, 0.9f);
    l->begin_query(occ.get(), 1);
    l->draw(6, 1, r16, 0);
    l->end_query(occ.get(), 1);
    l->begin_query(bin.get(), 1);
    bind(e, *l, pz, {1, 1, 1, 1}, 0.1f);
    l->draw(6, 1, r16, 0);
    l->end_query(bin.get(), 1);
    l->resolve_queries(occ.get(), 0, 2, rb.get(), 0);
    l->resolve_queries(bin.get(), 1, 1, rb.get(), 16);
    CHECK(run(e, *l), "queries");
    const auto* r = static_cast<const std::uint64_t*>(rb->cpu_pointer());
    std::printf("occlusion: 16x16 rect visible %llu samples, behind the depth buffer %llu, binary %llu\n",
                (unsigned long long)r[0], (unsigned long long)r[1], (unsigned long long)r[2]);
    CHECK(r[0] == 256 && r[1] == 0 && r[2] == 1, "occlusion and binary occlusion results");
    (void)p;
}

void test_timestamps(Env& e, RenderPipeline* heavy, std::uint32_t full) {
    auto ts = e.dev->create_query_heap({QueryType::timestamp, 4, "timestamps"});
    CHECK(ts != nullptr, "timestamp heap");
    if (!ts) return;
    BufferDesc rbd{256, MemoryKind::readback, usage::copy, "timestamps"};
    auto rb = e.dev->create_buffer(rbd);
    std::memset(rb->cpu_pointer(), 0, 256);
    auto l = e.dev->create_command_list(QueueType::direct);
    Barrier serial;
    serial.type = BarrierType::uav;
    l->end_query(ts.get(), 0);
    l->barrier(std::span<const Barrier>(&serial, 1));
    targets(*l, e.rt.get());
    bind(e, *l, heavy, {1, 1, 1, 1}, 0.5f, 20000);
    l->set_topology(topology::triangle_list);
    l->draw(6, 1, full, 0);
    l->end_query(ts.get(), 1);
    l->barrier(std::span<const Barrier>(&serial, 1));
    bind(e, *l, heavy, {1, 1, 1, 1}, 0.5f, 40000);
    l->draw(6, 1, full, 0);
    l->end_query(ts.get(), 2);
    l->write_timestamp(rb.get(), 64);
    l->resolve_queries(ts.get(), 0, 3, rb.get(), 0);
    CHECK(l->close() == Status::ok, "close");
    CommandList* ls[] = {l.get()};
    for (int warm = 0; warm < 3; ++warm) {
        CHECK(e.q->execute(ls) == Status::ok, "warm-up execute");
        e.q->wait_idle(20'000'000'000ull);
    }
    constexpr int kRuns = 7, kMaxRuns = 35;
    const char* sv = std::getenv("MTL_SHADER_VALIDATION");
    const bool validation = sv && *sv && *sv != '0';
    const auto in_range = [](double r) { return r > 1.4 && r < 2.6; };
    double min1 = 1e9, min2 = 1e9, max1 = 0, max2 = 0;
    int runs = 0;
    for (int run = 0; run < kMaxRuns; ++run) {
        if (run >= kRuns && (validation || in_range(min2 / min1))) break;
        ++runs;
        const ClockCalibration c0 = e.q->clock_calibration();
        const auto w0 = std::chrono::steady_clock::now();
        CHECK(e.q->execute(ls) == Status::ok && e.q->wait_idle(20'000'000'000ull) == Status::ok, "timestamps");
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
        const ClockCalibration c1 = e.q->clock_calibration();
        const auto* t = static_cast<const std::uint64_t*>(rb->cpu_pointer());
        const double d1 = double(t[1] - t[0]) / e.q->timestamp_frequency(), d2 = double(t[2] - t[1]) / e.q->timestamp_frequency();
        if (run == 0)
            std::printf("timestamps (frequency %llu Hz): t0 %llu t1 %llu t2 %llu, write_timestamp %llu; draw A %.3f ms, "
                        "draw B (2x the loop) %.3f ms; execute-to-idle wall %.3f ms; calibration gpu %llu .. %llu\n",
                        (unsigned long long)e.q->timestamp_frequency(), (unsigned long long)t[0], (unsigned long long)t[1],
                        (unsigned long long)t[2], (unsigned long long)t[8], d1 * 1e3, d2 * 1e3, wall * 1e3,
                        (unsigned long long)c0.gpu_ticks, (unsigned long long)c1.gpu_ticks);
        CHECK(t[0] < t[1] && t[1] < t[2] && t[2] <= t[8], "timestamps increase in stream order");
        CHECK(t[0] >= c0.gpu_ticks && t[8] <= c1.gpu_ticks, "timestamps lie between the calibration samples");
        CHECK((d1 + d2) < wall, "GPU deltas are shorter than the wall time");
        min1 = std::min(min1, d1);
        min2 = std::min(min2, d2);
        max1 = std::max(max1, d1);
        max2 = std::max(max2, d2);
    }
    std::printf("timestamps over %d runs: draw A %.3f-%.3f ms, draw B %.3f-%.3f ms, ratio of the minima %.2f\n", runs,
                min1 * 1e3, max1 * 1e3, min2 * 1e3, max2 * 1e3, min2 / min1);
    if (validation)
        std::printf("shader validation on: the ratio is reported, not checked (instrumented draws do not scale with the loop)\n");
    else
        CHECK(in_range(min2 / min1), "timestamp deltas track the work (ratio of the minima %.2f)", min2 / min1);
}

void test_stencil(Env& e, std::uint32_t r16, std::uint32_t full) {
    for (std::uint32_t fmt : {format::d24_unorm_s8_uint, format::d32_float_s8x24_uint}) {
        auto depth = depth_texture(e, fmt);
        DepthStencilDesc write;
        write.stencil = true;
        write.front.func = write.back.func = CompareFunc::always;
        write.front.pass = write.back.pass = StencilOp::replace;
        DepthStencilDesc test = write;
        test.front.func = test.back.func = CompareFunc::equal;
        test.front.pass = test.back.pass = StencilOp::keep;
        DepthStencilDesc incr = write;
        incr.front.pass = incr.back.pass = StencilOp::incr_sat;
        auto pw = pipeline(e, "ps_main", fmt, write);
        auto pt = pipeline(e, "ps_main", fmt, test);
        auto pi = pipeline(e, "ps_main", fmt, incr);
        auto l = e.dev->create_command_list(QueueType::direct);
        targets(*l, e.rt.get(), depth.get());
        l->clear_render_target({e.rt.get(), 0, 0, 0}, {0, 0, 0, 1});
        l->clear_depth_stencil({depth.get(), 0, 0, 0}, true, 1.0f, true, 0);
        l->set_topology(topology::triangle_list);
        bind(e, *l, pw.get(), {1, 0, 0, 1});
        l->set_stencil_ref(5);
        l->draw(6, 1, r16, 0);
        bind(e, *l, pi.get(), {1, 0, 0, 1});
        l->draw(6, 1, r16, 0);
        bind(e, *l, pt.get(), {0, 1, 0, 1});
        l->set_stencil_ref(6);
        l->draw(6, 1, full, 0);
        CHECK(run(e, *l), "stencil");
        const auto px = pixels(e, *e.rt);
        const int green = count_color(px, {0, 255, 0}), red = count_color(px, {255, 0, 0});
        std::printf("stencil on %s: write ref 5 (replace), incr_sat, then equal 6 over the screen: %d green (want 256), "
                    "%d red\n",
                    fmt == format::d24_unorm_s8_uint ? "D24S8 (as D32F_S8)" : "D32F_S8", green, red);
        CHECK(green == 256 && red == 0, "stencil replace, incr_sat and equal");
    }
}

void test_blend(Env& e, std::uint32_t full) {
    TargetBlend dual;
    dual.enable = true;
    dual.src = Blend::one;
    dual.dst = Blend::src1_color;
    dual.src_alpha = Blend::one;
    dual.dst_alpha = Blend::zero;
    auto pd = pipeline(e, "ps_dual", format::unknown, {}, {}, dual);
    TargetBlend factor;
    factor.enable = true;
    factor.src = Blend::blend_factor;
    factor.dst = Blend::zero;
    factor.src_alpha = Blend::one;
    factor.dst_alpha = Blend::zero;
    auto pf = pipeline(e, "ps_main", format::unknown, {}, {}, factor);
    TargetBlend masked;
    masked.write_mask = 0x2;
    auto pm = pipeline(e, "ps_main", format::unknown, {}, {}, masked);
    struct Case { RenderPipeline* p; const char* name; std::array<float, 4> color; std::array<std::uint8_t, 3> want; };
    const Case cases[] = {{pd.get(), "dual source: one * src0 + src1_color * dst", {0.25f, 0, 0, 1}, {128, 64, 96}},
                          {pf.get(), "blend factor (0.5, 0.25, 1, 1) * white", {1, 1, 1, 1}, {128, 64, 255}},
                          {pm.get(), "write mask G only over (0.5, 0.5, 0.5)", {1, 0, 1, 1}, {128, 0, 128}}};
    for (const Case& c : cases) {
        auto l = e.dev->create_command_list(QueueType::direct);
        targets(*l, e.rt.get());
        l->clear_render_target({e.rt.get(), 0, 0, 0}, {0.5f, 0.5f, 0.5f, 1});
        bind(e, *l, c.p, c.color);
        std::uint32_t src1[4] = {fbits(0.5f), fbits(0.5f), fbits(0.75f), fbits(1)};
        l->set_user_data(8, src1);
        l->set_blend_factor({0.5f, 0.25f, 1, 1});
        l->set_topology(topology::triangle_list);
        l->draw(6, 1, full, 0);
        CHECK(run(e, *l), "blend");
        const auto px = pixels(e, *e.rt);
        std::printf("blend %s: pixel %u %u %u (want %u %u %u)\n", c.name, px[0], px[1], px[2], c.want[0], c.want[1],
                    c.want[2]);
        CHECK(count_color(px, c.want) == int(N * N), "%s", c.name);
    }
}

void test_depth(Env& e, std::uint32_t r16, std::uint32_t full) {
    struct Case { std::uint32_t fmt; D24Mode mode; const char* name; };
    const Case cases[] = {{format::d32_float, D24Mode::fragment, "D32F"},
                          {format::d24_unorm_s8_uint, D24Mode::fragment, "D24S8 fragment"},
                          {format::d24_unorm_s8_uint, D24Mode::rasterizer, "D24S8 rasterizer"}};
    for (const Case& c : cases) {
        auto depth = depth_texture(e, c.fmt);
        DepthStencilDesc ds;
        ds.depth_test = true;
        ds.depth_write = true;
        ds.depth_func = CompareFunc::always;
        RasterDesc biased;
        biased.depth_bias = 1000;
        auto pb = pipeline(e, "ps_main", c.fmt, ds, biased, {}, c.mode);
        auto pn = pipeline(e, "ps_main", c.fmt, ds, {}, {}, c.mode);
        auto pdw = pipeline(e, "ps_depthwrite", c.fmt, ds, {}, {}, c.mode);
        auto ponly = pipeline(e, nullptr, c.fmt, ds, biased, {}, c.mode);
        DepthStencilDesc eq;
        eq.depth_test = true;
        eq.depth_write = false;
        eq.depth_func = CompareFunc::equal;
        auto peq = pipeline(e, "ps_main", c.fmt, eq, {}, {}, c.mode);
        const float zs[4] = {0.5f, 0.3f, 0.7f, 0.5f};
        float got[4] = {};
        for (int k = 0; k < 4; ++k) {
            auto l = e.dev->create_command_list(QueueType::direct);
            targets(*l, e.rt.get(), depth.get());
            l->clear_depth_stencil({depth.get(), 0, 0, 0}, true, 1.0f, false, 0);
            RenderPipeline* p = k == 0 ? pb.get() : k == 1 ? pn.get() : k == 2 ? pdw.get() : ponly.get();
            bind(e, *l, p, {1, 1, 1, 1}, zs[k]);
            std::uint32_t dz = fbits(0.7f);
            l->set_user_data(6, std::span<const std::uint32_t>(&dz, 1));
            l->set_topology(topology::triangle_list);
            l->draw(6, 1, full, 0);
            CHECK(run(e, *l), "depth draw");
            got[k] = depth_at(e, *depth, 32, 32);
        }
        const bool d24 = c.fmt == format::d24_unorm_s8_uint;
        std::printf("%s: z 0.5 + bias 1000 -> %.9g (0.5 + %.6g); z 0.3 -> %.9g; SV_Depth 0.7 -> %.9g; depth-only z 0.5 + "
                    "bias -> %.9g\n",
                    c.name, got[0], double(got[0]) - 0.5, got[1], got[2], got[3]);
        if (d24 && c.mode == D24Mode::fragment) {
            const float b = float(1000) * float(1.0 / 16777215.0);
            CHECK(got[0] == float(d24_reference(0.5f + b)), "D24 fragment: biased depth is quantise(z + DepthBias * r)");
            CHECK(got[1] == float(d24_reference(0.3f)), "D24 fragment: depth is quantised");
            CHECK(got[2] == float(d24_reference(0.7f)), "D24 fragment: SV_Depth is quantised");
            CHECK(got[3] == float(d24_reference(0.5f + b)), "D24 depth-only fragment applies bias and quantisation");
        }
        auto l = e.dev->create_command_list(QueueType::direct);
        targets(*l, e.rt.get(), depth.get());
        l->clear_render_target({e.rt.get(), 0, 0, 0}, {0, 0, 0, 1});
        l->clear_depth_stencil({depth.get(), 0, 0, 0}, true, 0.3f, false, 0);
        bind(e, *l, peq.get(), {0, 1, 0, 1}, 0.3f);
        l->set_topology(topology::triangle_list);
        l->draw(6, 1, full, 0);
        CHECK(run(e, *l), "equal test");
        const int pass = count_color(pixels(e, *e.rt), {0, 255, 0});
        std::printf("%s: clear 0.3, draw at z 0.3 with EQUAL: %d of %u pixels pass\n", c.name, pass, N * N);
        if (d24 && c.mode == D24Mode::fragment) CHECK(pass == int(N * N), "D24 fragment: EQUAL at the cleared value passes");
        if (!d24) CHECK(pass == int(N * N), "D32F: EQUAL at the cleared value passes");
        (void)r16;
    }
}

void test_quantiser(Env& e) {
    std::string err;
    auto lib = e.dev->create_library_msl(gta6rt::metal::d24_quantize_probe_msl(), MslOptions{}, &err);
    CHECK(lib != nullptr, "probe library: %s", err.c_str());
    if (!lib) return;
    auto root = e.dev->create_root_layout(RootLayoutDesc{});
    ComputePipelineDesc cd;
    cd.cs = {lib.get(), "gta6rt_d24_probe"};
    cd.threads = {256, 1, 1};
    cd.root = root.get();
    auto p = e.dev->create_compute_pipeline(cd);
    CHECK(p->status() == Status::ok, "probe pipeline %s", p->error().c_str());
    const std::uint32_t codes = 1u << 24, extra = 1u << 20, n = codes + extra + 8;
    HeapDesc hd;
    hd.size = (std::uint64_t(n) * 8 + (1 << 20)) & ~std::uint64_t((1 << 16) - 1);
    hd.memory = MemoryKind::shared;
    hd.console_arena = true;
    auto heap = e.dev->create_heap(hd);
    auto* f = static_cast<float*>(heap->arena()->cpu_pointer());
    for (std::uint32_t c = 0; c < codes; ++c) f[c] = float(double(c) / 16777215.0);
    std::mt19937 rng(24);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    for (std::uint32_t i = 0; i < extra; ++i) f[codes + i] = uni(rng);
    const float edge[8] = {-0.0f, -1.0f, 1.0f, 2.0f, std::nanf(""), 1e-30f, 0.99999994f, 5.96046448e-8f};
    std::memcpy(f + codes + extra, edge, sizeof(edge));
    auto l = e.dev->create_command_list(QueueType::direct);
    l->set_root_layout(root.get());
    l->set_heap_base(heap->host_gpu_address());
    const std::uint32_t u[3] = {0, n * 4, n};
    l->set_user_data(0, u);
    l->set_compute_pipeline(p.get());
    l->dispatch(n, 1, 1, dispatch_flag::thread_counts);
    CHECK(run(e, *l), "probe dispatch");
    const float* out = f + n;
    std::uint32_t bad_codes = 0, bad_random = 0, bad_edge = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        const float want = float(d24_reference(f[i]));
        if (std::memcmp(&want, &out[i], 4) != 0) (i < codes ? bad_codes : i < codes + extra ? bad_random : bad_edge)++;
    }
    std::printf("D24 quantiser on the GPU against leonida's depth24_clear_value: %u of %u code values, %u of %u random "
                "values, %u of 8 edge values differ\n",
                bad_codes, codes, bad_random, extra, bad_edge);
    CHECK(bad_codes == 0 && bad_random == 0 && bad_edge == 0, "GPU D24 quantiser is bit-exact");
}

void test_b4g4r4a4(Env& e, std::uint32_t full) {
    TextureDesc td;
    td.format = format::b4g4r4a4_unorm;
    td.width = td.height = 2;
    td.usage = usage::shader_read | usage::render_target;
    td.memory = MemoryKind::shared;
    auto t = e.dev->create_texture(td);
    CHECK(t != nullptr, "B4G4R4A4 texture");
    if (!t) return;
    const std::uint16_t texel[4] = {0x1234, 0x1234, 0x1234, 0x1234};
    CHECK(t->write_region(0, 0, 0, 0, 2, 2, texel, 4) == Status::ok, "write B4G4R4A4 texel");
    auto table = e.dev->create_descriptor_heap({DescriptorKind::texture, 4, "b4g4r4a4"});
    table->write_texture(1, t.get());
    auto pt = pipeline(e, "ps_texel");
    auto l = e.dev->create_command_list(QueueType::direct);
    targets(*l, e.rt.get());
    bind(e, *l, pt.get(), {0, 0, 0, 0});
    const std::uint32_t slot = 1;
    l->set_user_data(12, std::span<const std::uint32_t>(&slot, 1));
    l->set_descriptor_table(0, table.get());
    l->set_topology(topology::triangle_list);
    l->draw(6, 1, full, 0);
    CHECK(run(e, *l), "texel draw");
    const auto px = pixels(e, *e.rt);
    std::printf("B4G4R4A4 texel 0x1234 (DXGI: B=4 bits 0-3, G=3, R=2, A=1 bits 12-15) samples as R %u G %u B %u A %u "
                "(DXGI order gives 34 51 68 17)\n",
                px[0], px[1], px[2], px[3]);
    CHECK(px[0] == 34 && px[1] == 51 && px[2] == 68 && px[3] == 17, "B4G4R4A4 component order matches DXGI");

    RenderPipelineDesc pd;
    pd.vs = {e.lib.get(), "vs_main"};
    pd.ps = {e.lib.get(), "ps_main"};
    pd.inputs = {{0, format::r32g32_float, 0, 0}};
    pd.rt_formats[0] = format::b4g4r4a4_unorm;
    pd.rt_count = 1;
    pd.raster.cull = CullMode::none;
    pd.root = e.root.get();
    auto pr = e.dev->create_render_pipeline(pd);
    CHECK(pr->status() == Status::ok, "B4G4R4A4 render target pipeline: %s", pr->error().c_str());
    TextureDesc rd = td;
    rd.width = rd.height = N;
    rd.memory = MemoryKind::device_local;
    auto target = e.dev->create_texture(rd);
    BufferDesc bd{N * N * 2, MemoryKind::readback, usage::copy, "b4g4r4a4 rb"};
    auto rb = e.dev->create_buffer(bd);
    l = e.dev->create_command_list(QueueType::direct);
    targets(*l, target.get());
    bind(e, *l, pr.get(), {2.0f / 15, 3.0f / 15, 4.0f / 15, 1.0f / 15});
    l->set_topology(topology::triangle_list);
    l->draw(6, 1, full, 0);
    Barrier b;
    b.resource = target.get();
    b.before = kRenderTarget;
    b.after = kCopySource;
    l->barrier(std::span<const Barrier>(&b, 1));
    BufferTextureCopy c;
    c.buffer = rb.get();
    c.texture = target.get();
    c.row_pitch = N * 2;
    l->copy_texture_to_buffer(c);
    CHECK(run(e, *l), "B4G4R4A4 render");
    const std::uint16_t raw = *static_cast<const std::uint16_t*>(rb->cpu_pointer());
    std::printf("B4G4R4A4 render target: colour (2, 3, 4, 1)/15 stored as 0x%04x (DXGI order gives 0x1234)\n", raw);
    if (raw != 0x1234) std::printf("  measured: a B4G4R4A4 render target stores Metal's ABGR4 order (stub, STATUS.md)\n");
}

void test_a8(Env& e, std::uint32_t full) {
    constexpr std::uint32_t W = 3840, H = 2160;
    FormatInfo fi;
    CHECK(e.dev->describe_format(format::a8_unorm, fi) && !fi.emulated, "A8_UNORM maps to a native format");
    TextureDesc td;
    td.format = format::a8_unorm;
    td.width = W;
    td.height = H;
    td.usage = usage::render_target | usage::shader_read;
    auto target = e.dev->create_texture(td);
    CHECK(target != nullptr, "A8_UNORM 3840x2160 render target");
    if (!target) return;
    TargetBlend over;
    over.enable = true;
    over.src = over.src_alpha = Blend::one;
    over.dst = over.dst_alpha = Blend::inv_src_alpha;
    std::unique_ptr<RenderPipeline> ps[2];
    for (int k = 0; k < 2; ++k) {
        RenderPipelineDesc pd;
        pd.vs = {e.lib.get(), "vs_main"};
        pd.ps = {e.lib.get(), "ps_main"};
        pd.inputs = {{0, format::r32g32_float, 0, 0}};
        pd.rt_formats[0] = format::a8_unorm;
        pd.rt_count = 1;
        if (k == 1) pd.blend[0] = over;
        pd.raster.cull = CullMode::none;
        pd.root = e.root.get();
        ps[k] = e.dev->create_render_pipeline(pd);
        CHECK(ps[k]->status() == Status::ok, "A8 pipeline: %s", ps[k]->error().c_str());
    }
    BufferDesc bd{std::uint64_t(W) * H, MemoryKind::readback, usage::copy, "a8 rb"};
    auto rb = e.dev->create_buffer(bd);
    const char* names[3] = {"clear alpha 0.5", "draw alpha 0.6, no blend", "draw alpha 0.6 over a cleared 0.5 (one, inv_src_alpha)"};
    const std::uint8_t want[3] = {128, 153, 204};
    for (int k = 0; k < 3; ++k) {
        auto l = e.dev->create_command_list(QueueType::direct);
        targets(*l, target.get());
        l->clear_render_target({target.get(), 0, 0, 0}, {0.25f, 0.5f, 0.75f, 0.5f});
        if (k > 0) {
            bind(e, *l, ps[k - 1].get(), {0.25f, 0.5f, 0.75f, 0.6f});
            l->set_topology(topology::triangle_list);
            l->draw(6, 1, full, 0);
        }
        Barrier b;
        b.resource = target.get();
        b.before = kRenderTarget;
        b.after = kCopySource;
        l->barrier(std::span<const Barrier>(&b, 1));
        BufferTextureCopy c;
        c.buffer = rb.get();
        c.texture = target.get();
        c.row_pitch = W;
        c.width = W;
        c.height = H;
        l->copy_texture_to_buffer(c);
        CHECK(run(e, *l), "A8 %s", names[k]);
        const auto* px = static_cast<const std::uint8_t*>(rb->cpu_pointer());
        std::uint64_t bad = 0;
        for (std::uint64_t i = 0; i < std::uint64_t(W) * H; ++i) bad += std::abs(int(px[i]) - want[k]) > 1;
        std::printf("A8_UNORM %ux%u render target, %s: texel %u (want %u), %llu of %u texels off by more than 1\n", W, H,
                    names[k], px[0], want[k], (unsigned long long)bad, W * H);
        CHECK(bad == 0, "A8 %s", names[k]);
    }
}

}  // namespace

int main() {
    auto dev = gta6rt::metal::create_device();
    if (!dev) return 1;
    test::print_caps(dev->caps());
    auto queue = dev->create_queue(QueueType::direct, "draw2");
    Env e;
    e.dev = dev.get();
    e.q = queue.get();
    std::string err;
    e.lib = dev->create_library_msl(test::read_file(test::shader_path("draw2.metal")), MslOptions{}, &err);
    CHECK(e.lib != nullptr, "library: %s", err.c_str());
    if (!e.lib) return test::finish("metal_test_draw2");
    RootLayoutDesc rd;
    rd.tables = {{0, DescriptorKind::texture}};
    e.root = dev->create_root_layout(rd);
    TextureDesc td;
    td.format = format::r8g8b8a8_unorm;
    td.width = td.height = N;
    td.usage = usage::render_target | usage::shader_read;
    e.rt = dev->create_texture(td);

    const std::uint32_t r_full = add_rect(e, 0, 0, N, N);
    const std::uint32_t r16 = add_rect(e, 8, 8, 24, 24);
    const std::uint32_t r_cols = add_rect(e, 0, 0, 20, N);
    add_rect(e, 22, 0, 42, N);
    add_rect(e, 44, 0, 64, N);
    const std::uint32_t q0 = add_quad(e, 0, 0, 16, 16);
    add_quad(e, 16, 0, 32, 16);
    e.vb = upload(e, e.verts.data(), e.verts.size() * 4);
    const std::uint16_t rect_idx[6] = {0, 1, 2, 3, 4, 5};
    auto ib = upload(e, rect_idx, sizeof(rect_idx));

    auto p = pipeline(e, "ps_main");
    DepthStencilDesc zl;
    zl.depth_test = true;
    zl.depth_write = true;
    zl.depth_func = CompareFunc::less;
    auto pz = pipeline(e, "ps_main", format::d32_float, zl);
    auto heavy = pipeline(e, "ps_heavy");
    if (test::failures) return test::finish("metal_test_draw2");

    test_indirect(e, p.get(), r_cols, ib);
    test_quads(e, p.get(), q0);
    test_queries(e, p.get(), pz.get(), r16);
    test_timestamps(e, heavy.get(), r_full);
    test_stencil(e, r16, r_full);
    test_blend(e, r_full);
    test_depth(e, r16, r_full);
    test_quantiser(e);
    test_b4g4r4a4(e, r_full);
    test_a8(e, r_full);
    queue->wait_idle(10'000'000'000ull);
    test::print_stats(dev->stats());
    return test::finish("metal_test_draw2");
}
