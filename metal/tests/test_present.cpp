#include "test_util.h"

#include <cmath>

using namespace gta6rt::gpu;

int main() {
    auto dev = gta6rt::metal::create_device();
    if (!dev) return 1;
    test::print_caps(dev->caps());
    const std::uint32_t W = 640, H = 360;
    auto win = gta6rt::metal::create_window("gta6rt_metal: cleared frame", W, H, !test::headless());
    CHECK(win != nullptr, "window");
    if (!win) return test::finish("metal_test_present");
    std::printf("window: %s, layer %p\n", win->visible() ? "visible NSWindow" : "layer only", win->layer());
    auto queue = dev->create_queue(QueueType::direct, "present");
    SwapchainDesc sd;
    sd.layer = win->layer();
    sd.width = W;
    sd.height = H;
    sd.buffer_count = 2;
    sd.format = format::r8g8b8a8_unorm;
    sd.keep_last_frame = true;
    auto sc = dev->create_swapchain(sd);
    CHECK(sc && sc->buffer_count() == 2, "swapchain");
    if (!sc) return test::finish("metal_test_present");

    const std::array<std::array<float, 4>, 3> colors = {{{0.25f, 0.5f, 0.75f, 1.0f}, {1, 0, 0, 1}, {0, 1, 0.5f, 1}}};
    auto list = dev->create_command_list(QueueType::direct);
    for (std::uint32_t frame = 0; frame < 6; ++frame) {
        const auto& c = colors[frame % colors.size()];
        Texture* bb = sc->buffer(sc->current_index());
        list->reset();
        list->clear_render_target(TargetView{bb, 0, 0, 0}, c);
        CHECK(list->close() == Status::ok, "close");
        CommandList* lists[] = {list.get()};
        CHECK(queue->execute(lists) == Status::ok, "execute");
        const Status ps = queue->present(sc.get(), 1);
        CHECK(ps == Status::ok, "present %s", status_name(ps));
        std::vector<std::uint8_t> px;
        std::uint32_t w = 0, h = 0;
        CHECK(sc->read_last_frame(px, w, h) == Status::ok && w == W && h == H, "read frame %ux%u", w, h);
        if (px.size() >= std::size_t(W) * H * 4) {
            auto to8 = [](float f) { return int(std::lround(f * 255.0f)); };
            int bad = 0;
            for (std::size_t i = 0; i < std::size_t(W) * H; ++i) {
                const std::uint8_t* p = &px[i * 4];
                if (std::abs(p[2] - to8(c[0])) > 1 || std::abs(p[1] - to8(c[1])) > 1 || std::abs(p[0] - to8(c[2])) > 1 ||
                    p[3] != 255)
                    ++bad;
            }
            CHECK(bad == 0, "frame %u: %d pixels differ from the clear colour", frame, bad);
            if (frame == 0)
                std::printf("frame 0 pixel (0,0) BGRA = %u %u %u %u, expected %d %d %d 255\n", px[0], px[1], px[2], px[3],
                            to8(c[2]), to8(c[1]), to8(c[0]));
        }
        win->pump_events(0.05);
    }
    CHECK(sc->presented() == 6, "presented %llu", (unsigned long long)sc->presented());
    queue->wait_idle(5'000'000'000ull);
    test::print_stats(dev->stats());
    return test::finish("metal_test_present");
}
