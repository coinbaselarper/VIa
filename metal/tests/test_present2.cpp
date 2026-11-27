#include "test_util.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

using namespace gta6rt::gpu;

namespace {

constexpr std::uint32_t W = 640, H = 360, SW = 320, SH = 180;

std::array<float, 4> scene(std::uint32_t x, std::uint32_t y) {
    return {float(x % 64) / 63.0f, float(y % 32) / 31.0f, float((x + y) % 16) / 15.0f, 1.0f};
}

std::array<float, 4> ui(std::uint32_t x, std::uint32_t y) {
    if (x >= 100 && x < 300 && y >= 50 && y < 150) return {0.4f, 0.2f, 0.0f, 0.5f};
    if (x >= 400 && x < 500 && y >= 200 && y < 300) return {0.0f, 0.0f, 0.75f, 0.75f};
    return {0, 0, 0, 0};
}

std::uint8_t u8(float f) { return std::uint8_t(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); }

std::unique_ptr<Texture> make(Device& dev, std::uint32_t w, std::uint32_t h, auto fn) {
    TextureDesc td;
    td.format = format::r8g8b8a8_unorm;
    td.width = w;
    td.height = h;
    td.usage = usage::shader_read;
    td.memory = MemoryKind::shared;
    auto t = dev.create_texture(td);
    std::vector<std::uint8_t> px(std::size_t(w) * h * 4);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const auto c = fn(x, y);
            for (int k = 0; k < 4; ++k) px[(std::size_t(y) * w + x) * 4 + k] = u8(c[k]);
        }
    t->write_region(0, 0, 0, 0, w, h, px.data(), w * 4);
    return t;
}

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

}  // namespace

int main() {
    auto dev = gta6rt::metal::create_device();
    if (!dev) return 1;
    test::print_caps(dev->caps());
    std::printf("display refresh %.1f Hz\n", dev->caps().display_refresh_hz);
    auto win = gta6rt::metal::create_window("gta6rt_metal: planes", W, H, !test::headless());
    CHECK(win != nullptr, "window");
    if (!win) return test::finish("metal_test_present2");
    std::printf("window: %s\n", win->visible() ? "visible NSWindow" : "layer only");
    auto q = dev->create_queue(QueueType::direct, "present2");
    SwapchainDesc sd;
    sd.layer = win->layer();
    sd.width = W;
    sd.height = H;
    sd.keep_last_frame = true;
    sd.frame_events = true;
    auto sc = dev->create_swapchain(sd);
    CHECK(sc != nullptr, "swapchain");
    if (!sc) return test::finish("metal_test_present2");

    auto s = make(*dev, SW, SH, scene);
    auto o = make(*dev, W, H, ui);
    auto small_ui = make(*dev, SW, SH, [](std::uint32_t x, std::uint32_t y) { return ui(x * 2, y * 2); });

    struct Case { const char* name; PresentDesc desc; };
    PresentDesc two;
    two.plane_count = 2;
    two.planes[0] = {s.get(), {}, {}, Filter::point, PlaneBlend::opaque};
    two.planes[1] = {o.get(), {}, {}, Filter::point, PlaneBlend::premultiplied};
    PresentDesc placed = two;
    placed.planes[1] = {small_ui.get(), {}, {320, 180, 640, 360}, Filter::point, PlaneBlend::premultiplied};
    const Case cases[] = {{"scene 320x180 scaled to 640x360 + premultiplied UI plane", two},
                          {"same + UI plane 320x180 placed at (320,180)-(640,360)", placed}};
    for (int ci = 0; ci < 2; ++ci) {
        const Case& c = cases[ci];
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(q->present_planes(sc.get(), c.desc) == Status::ok, "present_planes");
        const double enc = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::vector<std::uint8_t> bgra;
        std::uint32_t w = 0, h = 0;
        CHECK(sc->read_last_frame(bgra, w, h) == Status::ok && w == W && h == H, "read the drawable");
        int bad = 0, checked = 0;
        for (std::uint32_t y = 0; y < H; ++y)
            for (std::uint32_t x = 0; x < W; ++x) {
                const auto sv = scene(x / 2, y / 2);
                std::array<float, 4> ov{};
                if (ci == 0) {
                    ov = ui(x, y);
                } else if (x >= 320 && y >= 180) {
                    ov = ui((x - 320) * 2, (y - 180) * 2);
                }
                std::array<float, 3> want;
                for (int k = 0; k < 3; ++k) {
                    const float sq = u8(sv[k]) / 255.0f, oq = u8(ov[k]) / 255.0f, oa = u8(ov[3]) / 255.0f;
                    want[k] = oq + sq * (1 - oa);
                }
                const std::uint8_t* p = &bgra[(std::size_t(y) * W + x) * 4];
                ++checked;
                if (std::abs(p[2] - u8(want[0])) > 1 || std::abs(p[1] - u8(want[1])) > 1 || std::abs(p[0] - u8(want[2])) > 1) {
                    if (bad < 3)
                        std::printf("  (%u,%u) BGRA %u %u %u, want RGB %u %u %u\n", x, y, p[0], p[1], p[2], u8(want[0]),
                                    u8(want[1]), u8(want[2]));
                    ++bad;
                }
            }
        std::printf("%s: %d of %d drawable pixels differ from o.rgb + s.rgb * (1 - o.a) by more than 1; present call %.3f ms\n",
                    c.name, bad, checked, enc * 1e3);
        CHECK(bad == 0, "%s", c.name);
    }

    if (!win->visible()) {
        std::printf("pacing not measured: layer-only window (no presented times)\n");
    } else {
        const double hz = dev->caps().display_refresh_hz > 0 ? dev->caps().display_refresh_hz : 60.0;
        for (std::uint32_t interval : {1u, 2u}) {
            PresentDesc d = two;
            d.sync_interval = interval;
            const int frames = interval == 1 ? 120 : 60;
            const FrameStats before = sc->frame_stats();
            const auto t0 = std::chrono::steady_clock::now();
            for (int f = 0; f < frames; ++f) {
                CHECK(q->present_planes(sc.get(), d) == Status::ok, "present");
                win->pump_events(0.0);
            }
            q->wait_idle(10'000'000'000ull);
            win->pump_events(0.2);
            const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const FrameStats st = sc->frame_stats();
            std::vector<double> iv = st.recent_intervals;
            const std::uint64_t shown = st.displayed - before.displayed;
            const std::size_t n = std::min<std::size_t>({iv.size(), std::size_t(frames - 10),
                                                         std::size_t(shown > 1 ? shown - 1 : 0)});
            iv.erase(iv.begin(), iv.end() - std::ptrdiff_t(n));
            std::printf("sync interval %u: %d presents in %.3f s (%.1f per second), %llu shown; median on-screen interval "
                        "%.3f ms (target %.3f ms), min %.3f max %.3f\n",
                        interval, frames, wall, frames / wall, (unsigned long long)(st.displayed - before.displayed),
                        median(iv) * 1e3, interval * 1e3 / hz, iv.empty() ? 0 : *std::min_element(iv.begin(), iv.end()) * 1e3,
                        iv.empty() ? 0 : *std::max_element(iv.begin(), iv.end()) * 1e3);
            if (iv.empty())
                std::printf("sync interval %u: pacing not measured (no presents of this phase reached the screen)\n",
                            interval);
            else
                CHECK(std::abs(median(iv) - interval / hz) < 0.25 / hz, "on-screen interval follows the sync interval");
        }
    }
    std::vector<double> ticks;
    auto last = std::chrono::steady_clock::now();
    for (int i = 0; i < 61; ++i) {
        CHECK(sc->wait_frame_event(1'000'000'000ull) == Status::ok, "frame event");
        const auto now = std::chrono::steady_clock::now();
        if (i) ticks.push_back(std::chrono::duration<double>(now - last).count());
        last = now;
    }
    std::printf("frame events: %zu waits, median period %.3f ms (display %.1f Hz -> %.3f ms), %llu ticks so far\n",
                ticks.size(), median(ticks) * 1e3, dev->caps().display_refresh_hz,
                dev->caps().display_refresh_hz > 0 ? 1e3 / dev->caps().display_refresh_hz : 0.0,
                (unsigned long long)sc->frame_stats().frame_events);
    const double hz = dev->caps().display_refresh_hz > 0 ? dev->caps().display_refresh_hz : 60.0;
    CHECK(std::abs(median(ticks) - 1.0 / hz) < 0.25 / hz, "frame events tick at the display rate");
    q->wait_idle(5'000'000'000ull);
    test::print_stats(dev->stats());
    return test::finish("metal_test_present2");
}
