#include "test_util.h"

#include <cmath>
#include <cstring>

using namespace gta6rt::gpu;

namespace {

const std::uint32_t W = 256, H = 256, T = 4;
const std::array<float, 4> kClear = {0.0f, 0.0f, 0.0f, 1.0f};

std::array<std::uint8_t, 4> texel(std::uint32_t x, std::uint32_t y) {
    return {std::uint8_t(x * 64 + 32), std::uint8_t(y * 64 + 16), std::uint8_t(200 - x * 10 - y), 255};
}

bool inside(float px, float py) {
    const float ax = 25.6f, ay = 230.4f, bx = 230.4f, by = 230.4f, cx = 128.0f, cy = 25.6f;
    auto edge = [](float x0, float y0, float x1, float y1, float x, float y) {
        return (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0);
    };
    const float e0 = edge(ax, ay, bx, by, px, py), e1 = edge(bx, by, cx, cy, px, py), e2 = edge(cx, cy, ax, ay, px, py);
    return (e0 <= 0 && e1 <= 0 && e2 <= 0) || (e0 >= 0 && e1 >= 0 && e2 >= 0);
}

}  // namespace

int main() {
    auto dev = gta6rt::metal::create_device();
    if (!dev) return 1;
    test::print_caps(dev->caps());
    auto win = gta6rt::metal::create_window("gta6rt_metal: bindless triangle", W, H, !test::headless());
    CHECK(win != nullptr, "window");
    if (!win) return test::finish("metal_test_triangle");
    std::printf("window: %s\n", win->visible() ? "visible NSWindow" : "layer only");
    auto queue = dev->create_queue(QueueType::direct, "triangle");

    SwapchainDesc sd;
    sd.layer = win->layer();
    sd.width = W;
    sd.height = H;
    sd.format = format::r8g8b8a8_unorm;
    sd.keep_last_frame = true;
    auto sc = dev->create_swapchain(sd);
    CHECK(sc != nullptr, "swapchain");
    if (!sc) return test::finish("metal_test_triangle");

    TextureDesc td;
    td.format = format::r8g8b8a8_unorm;
    td.width = T;
    td.height = T;
    td.usage = usage::shader_read;
    td.label = "checker";
    auto tex = dev->create_texture(td);
    BufferDesc upd{T * T * 4, MemoryKind::upload, usage::copy, "texture upload"};
    auto up = dev->create_buffer(upd);
    auto* tp = static_cast<std::uint8_t*>(up->cpu_pointer());
    for (std::uint32_t y = 0; y < T; ++y)
        for (std::uint32_t x = 0; x < T; ++x) std::memcpy(tp + (y * T + x) * 4, texel(x, y).data(), 4);

    SamplerDesc smp;
    smp.min = smp.mag = smp.mip = Filter::point;
    smp.u = smp.v = smp.w = AddressMode::clamp;
    auto sampler = dev->create_sampler(smp);

    const std::uint32_t kTexSlot = 37, kSmpSlot = 5;
    auto images = dev->create_descriptor_heap({DescriptorKind::texture, 1024, "binding 0: 2D images"});
    auto samplers = dev->create_descriptor_heap({DescriptorKind::sampler, 16, "binding 1: samplers"});
    CHECK(images && samplers, "descriptor heaps");
    CHECK(images->write_texture(kTexSlot, tex.get()) == Status::ok, "write texture descriptor");
    CHECK(samplers->write_sampler(kSmpSlot, sampler.get()) == Status::ok, "write sampler descriptor");
    CHECK(images->read_raw(kTexSlot) != 0 && images->read_raw(kTexSlot - 1) == 0, "texture handle written");
    std::printf("descriptor slot %u raw 0x%llx, sampler slot %u raw 0x%llx, stride %u\n", kTexSlot,
                (unsigned long long)images->read_raw(kTexSlot), kSmpSlot,
                (unsigned long long)samplers->read_raw(kSmpSlot), images->stride());

    RootLayoutDesc rd;
    rd.tables = {{0, DescriptorKind::texture}, {1, DescriptorKind::sampler}};
    auto root = dev->create_root_layout(rd);

    const std::string src = test::read_file(test::shader_path("triangle.metal"));
    std::string err;
    auto lib = dev->create_library_msl(src, MslOptions{}, &err);
    CHECK(lib != nullptr, "MSL library: %s", err.c_str());
    if (!lib) return test::finish("metal_test_triangle");

    RenderPipelineDesc pd;
    pd.vs = {lib.get(), "main0"};
    pd.ps = {lib.get(), "ps_main"};
    pd.inputs = {{0, format::r32g32_float, 0, 0}, {1, format::r32g32_float, 0, 8}};
    pd.rt_formats[0] = format::r8g8b8a8_unorm;
    pd.rt_count = 1;
    pd.raster.cull = CullMode::none;
    pd.root = root.get();
    pd.mode = CompileMode::lazy;
    const auto before = dev->stats();
    std::vector<std::unique_ptr<RenderPipeline>> unused;
    for (int i = 0; i < 3; ++i) unused.push_back(dev->create_render_pipeline(pd));
    auto pso = dev->create_render_pipeline(pd);
    CHECK(pso->state() == PipelineState::deferred, "lazy pipeline is not compiled at creation");

    struct V {
        float x, y, u, v;
    };
    const V verts[] = {{-0.8f, -0.8f, 0.1f, 0.9f}, {0.8f, -0.8f, 0.9f, 0.9f}, {0.0f, 0.8f, 0.5f, 0.1f}};
    BufferDesc vbd{sizeof(verts), MemoryKind::upload, usage::vertex, "vertices"};
    auto vb = dev->create_buffer(vbd);
    std::memcpy(vb->cpu_pointer(), verts, sizeof(verts));
    const std::uint16_t idx[] = {0, 1, 2, 0};
    BufferDesc ibd{sizeof(idx), MemoryKind::upload, usage::index, "indices"};
    auto ib = dev->create_buffer(ibd);
    std::memcpy(ib->cpu_pointer(), idx, sizeof(idx));

    auto list = dev->create_command_list(QueueType::direct);
    BufferTextureCopy c;
    c.buffer = up.get();
    c.texture = tex.get();
    c.row_pitch = T * 4;
    list->copy_buffer_to_texture(c);
    Barrier b;
    b.resource = tex.get();
    b.before = 0x400;
    b.after = 0x80;
    list->barrier(std::span<const Barrier>(&b, 1));

    Texture* bb = sc->buffer(sc->current_index());
    TargetView rt{bb, 0, 0, 0};
    VertexBufferView vbv{vb->gpu_va(), sizeof(verts), sizeof(V)};
    auto record = [&](TargetView target, float ox) {
        list->set_render_targets(std::span<const TargetView>(&target, 1), nullptr);
        list->clear_render_target(target, kClear);
        list->set_pipeline(pso.get());
        list->set_root_layout(root.get());
        std::uint32_t user[4] = {kTexSlot, kSmpSlot, 0, 0};
        std::memcpy(&user[2], &ox, 4);
        list->set_user_data(0, user);
        list->set_descriptor_table(0, images.get());
        list->set_descriptor_table(1, samplers.get());
        list->set_vertex_buffers(0, std::span<const VertexBufferView>(&vbv, 1));
        list->set_index_buffer({ib->gpu_va(), sizeof(idx), format::r16_uint});
        list->set_topology(topology::triangle_list);
        list->draw_indexed(3, 1, 0, 0, 0);
    };
    record(rt, 0.0f);
    CHECK(list->close() == Status::ok, "close");
    CommandList* lists[] = {list.get()};
    CHECK(queue->execute(lists) == Status::ok, "execute");
    CHECK(queue->present(sc.get(), 1) == Status::ok, "present");
    CHECK(pso->state() == PipelineState::ready, "lazy pipeline compiled at first draw (%s)", pso->error().c_str());
    const auto after = dev->stats();
    CHECK(after.pipelines_compiled - before.pipelines_compiled == 1, "one of four lazy pipelines compiled (%llu)",
          (unsigned long long)(after.pipelines_compiled - before.pipelines_compiled));
    CHECK(after.clears_folded - before.clears_folded == 1, "clear folded into the draw's render pass");
    std::printf("pipeline compiled in %.3f s at first use\n", pso->compile_seconds());
    for (const auto& e : pso->reflection().plan)
        std::printf("  plan: stage %u buffer(%u) what %u binding %u %s\n", e.stage, e.index, unsigned(e.what), e.binding,
                    e.note.c_str());
    CHECK(pso->reflection().mismatches.empty(), "binding plan has no mismatch");
    for (const auto& m : pso->reflection().mismatches) std::printf("  mismatch: %s\n", m.c_str());

    std::vector<std::uint8_t> px;
    std::uint32_t w = 0, h = 0;
    CHECK(sc->read_last_frame(px, w, h) == Status::ok && w == W && h == H, "drawable readback");
    std::vector<std::uint8_t> bbpx;
    CHECK(test::read_texture(*dev, *queue, *bb, 4, bbpx), "back buffer readback");
    int checked = 0, bad = 0, bad_present = 0, inside_n = 0;
    for (std::uint32_t y = 0; y < H; y += 3) {
        for (std::uint32_t x = 0; x < W; x += 3) {
            const float fx = x + 0.5f, fy = y + 0.5f;
            const float m = 1.5f;
            if (inside(fx - m, fy - m) != inside(fx + m, fy + m) || inside(fx - m, fy + m) != inside(fx + m, fy - m))
                continue;
            const bool in = inside(fx, fy);
            std::array<std::uint8_t, 4> want;
            if (in) {
                const float ndc_x = fx / W * 2 - 1, ndc_y = 1 - fy / H * 2;
                const float bx = (ndc_x - -0.8f), by = (ndc_y - -0.8f);
                const float wc = by / 1.6f, wb = (bx - 0.8f * wc) / 1.6f, wa = 1 - wb - wc;
                const float u = wa * 0.1f + wb * 0.9f + wc * 0.5f, v = wa * 0.9f + wb * 0.9f + wc * 0.1f;
                const float tu = u * T, tv = v * T;
                if (std::fabs(tu - std::round(tu)) < 0.02f || std::fabs(tv - std::round(tv)) < 0.02f) continue;
                want = texel(std::uint32_t(tu), std::uint32_t(tv));
                ++inside_n;
            } else {
                want = {0, 0, 0, 255};
            }
            ++checked;
            const std::uint8_t* p = &bbpx[(y * W + x) * 4];
            if (p[0] != want[0] || p[1] != want[1] || p[2] != want[2] || p[3] != want[3]) {
                if (bad < 5)
                    std::printf("  back buffer (%u,%u) = %u %u %u %u, want %u %u %u %u\n", x, y, p[0], p[1], p[2], p[3],
                                want[0], want[1], want[2], want[3]);
                ++bad;
            }
            const std::uint8_t* d = &px[(y * W + x) * 4];
            if (d[2] != p[0] || d[1] != p[1] || d[0] != p[2] || d[3] != p[3]) ++bad_present;
        }
    }
    std::printf("checked %d pixels (%d inside the triangle): %d wrong in the back buffer, %d drawable pixels differ "
                "from it\n",
                checked, inside_n, bad, bad_present);
    CHECK(bad == 0 && inside_n > 100, "triangle sampled through the argument buffer");
    CHECK(bad_present == 0, "presented drawable equals the back buffer");
    for (int i = 0; i < 10; ++i) {
        list->reset();
        record(TargetView{sc->buffer(sc->current_index()), 0, 0, 0}, 0.15f * std::sin(i * 0.6f));
        CHECK(list->close() == Status::ok, "close");
        CHECK(queue->execute(lists) == Status::ok, "execute frame %d", i);
        CHECK(queue->present(sc.get(), 1) == Status::ok, "present frame %d", i);
        win->pump_events(0.03);
    }
    queue->wait_idle(5'000'000'000ull);
    test::print_stats(dev->stats());
    return test::finish("metal_test_triangle");
}
