#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "gta6rt_hud/hud.h"
#include "hud_model.h"

#if __has_include("../../shims_gdk/src/gameinput_pad.h")
#include "../../shims_gdk/src/gameinput_pad.h"
#define HAVE_PAD_HEADER 1
#endif

extern "C" gta6rt_hud_stats* hud_c_block(void);
extern "C" void hud_c_produce(unsigned frames);

using namespace gta6rt::hud;

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        ++g_checks;                                                               \
        if (!(c)) {                                                               \
            ++g_fail;                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);              \
        }                                                                         \
    } while (0)

static_assert(__atomic_always_lock_free(8, 0), "64-bit atomics must be lock free");
static_assert(sizeof(gta6rt_hud_stats) % 8 == 0, "block size");

#if defined(__x86_64__)
__attribute__((ms_abi)) static void ms_abi_producer(unsigned frames) {
    for (unsigned i = 0; i < frames; ++i) {
        gta6rt_hud_add_draws(7);
        gta6rt_hud_note_present();
    }
}
#endif

static void test_inactive_is_noop() {
    gta6rt_hud_stats* b = gta6rt_hud_stats_block();
    std::memset(b, 0, sizeof *b);
    gta6rt_hud_set_active(0);
    gta6rt_hud_note_present();
    gta6rt_hud_add_draws(5);
    gta6rt_hud_note_submission(1000);
    gta6rt_hud_note_gpu_ns(10);
    CHECK(b->presents == 0 && b->draws == 0 && b->submissions == 0 && b->gpu_ns == 0 && b->have == 0);
}

static void test_c_and_cpp_share_block() {
    gta6rt_hud_stats* b = gta6rt_hud_stats_block();
    CHECK(hud_c_block() == b);
    std::memset(b, 0, sizeof *b);
    gta6rt_hud_set_active(1);
    hud_c_produce(5);
    CHECK(b->presents == 5 && b->draws == 15);
    gta6rt_hud_set_active(0);
}

static void test_activation_clears_stale() {
    gta6rt_hud_stats* b = gta6rt_hud_stats_block();
    std::memset(b, 0, sizeof *b);
    gta6rt_hud_set_active(1);
    gta6rt_hud_note_present();
    gta6rt_hud_note_present();
    gta6rt_hud_set_active(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    gta6rt_hud_set_active(1);
    CHECK(b->last_present_ns == 0 && b->frame_ns_max == 0);
    const uint64_t intervals = b->frame_intervals;
    gta6rt_hud_note_present();
    CHECK(b->frame_intervals == intervals);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    gta6rt_hud_note_present();
    CHECK(b->frame_intervals == intervals + 1);
    CHECK(b->frame_ns_max < 100000000ull);
    gta6rt_hud_set_active(0);
}

static void test_threads() {
    gta6rt_hud_stats* b = gta6rt_hud_stats_block();
    std::memset(b, 0, sizeof *b);
    gta6rt_hud_set_active(1);
    constexpr int kThreads = 4, kIters = 200000;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t)
        ts.emplace_back([] {
            for (int i = 0; i < kIters; ++i) {
                gta6rt_hud_add_draws(2);
                gta6rt_hud_add_dispatches(1);
                gta6rt_hud_note_submission(10);
            }
        });
    for (auto& t : ts) t.join();
    CHECK(b->draws == uint64_t(kThreads) * kIters * 2);
    CHECK(b->dispatches == uint64_t(kThreads) * kIters);
    CHECK(b->submissions == uint64_t(kThreads) * kIters);
    CHECK(b->exec_cpu_ns == uint64_t(kThreads) * kIters * 10);
    CHECK((b->have & GTA6RT_HUD_HAVE_SUBMISSIONS) && (b->have & GTA6RT_HUD_HAVE_DISPATCHES));
}

static void test_present_interval() {
    gta6rt_hud_stats* b = gta6rt_hud_stats_block();
    std::memset(b, 0, sizeof *b);
    gta6rt_hud_set_active(1);
    gta6rt_hud_note_present();
    CHECK(b->presents == 1 && b->frame_intervals == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    gta6rt_hud_note_present();
    CHECK(b->presents == 2 && b->frame_intervals == 1);
    CHECK(b->frame_ns_sum >= 19000000 && b->frame_ns_max == b->frame_ns_sum);
}

static void test_strings() {
    gta6rt_hud_stats* b = gta6rt_hud_stats_block();
    std::memset(b, 0, sizeof *b);
    std::atomic<bool> stop{false};
    std::atomic<int> torn{0}, reads{0};
    std::thread reader([&] {
        char out[GTA6RT_HUD_STRING_LEN];
        while (!stop.load()) {
            if (gta6rt_hud_get_string(&b->state, out) && out[0]) {
                ++reads;
                for (const char* p = out; *p; ++p)
                    if (*p != out[0]) {
                        ++torn;
                        break;
                    }
            }
        }
    });
    std::vector<std::thread> writers;
    for (int w = 0; w < 3; ++w)
        writers.emplace_back([w] {
            char text[GTA6RT_HUD_STRING_LEN];
            std::memset(text, 'a' + w, sizeof text - 1);
            text[sizeof text - 1] = 0;
            for (int i = 0; i < 300000; ++i) gta6rt_hud_set_state(text);
        });
    for (auto& w : writers) w.join();
    stop = true;
    reader.join();
    CHECK(torn.load() == 0);
    CHECK(reads.load() > 0);
    gta6rt_hud_set_state("compiling 132 pipelines");
    char out[GTA6RT_HUD_STRING_LEN];
    CHECK(gta6rt_hud_get_string(&b->state, out) == 1);
    CHECK(std::string(out) == "compiling 132 pipelines");
    std::string long_text(200, 'x');
    gta6rt_hud_set_state(long_text.c_str());
    gta6rt_hud_get_string(&b->state, out);
    CHECK(std::strlen(out) == GTA6RT_HUD_STRING_LEN - 1);
}

static void test_ms_abi() {
#if defined(__x86_64__)
    gta6rt_hud_stats* b = gta6rt_hud_stats_block();
    std::memset(b, 0, sizeof *b);
    gta6rt_hud_set_active(1);
    std::thread t([] { ms_abi_producer(1000); });
    t.join();
    CHECK(b->presents == 1000 && b->draws == 7000);
#else
    ++g_checks;
#endif
}

static Raw frame_raw(double t, uint64_t presents, uint64_t intervals, uint64_t sum_ns, uint64_t max_ns, uint64_t draws) {
    Raw r;
    r.t_s = t;
    r.presents = presents;
    r.frame_intervals = intervals;
    r.frame_ns_sum = sum_ns;
    r.frame_ns_max = max_ns;
    r.draws = draws;
    r.ram = 24ull << 30;
    r.footprint = uint64_t(6.2 * double(1ull << 30));
    return r;
}

static void test_sampler() {
    Sampler s;
    Metrics m;
    uint64_t presents = 0, intervals = 0, sum = 0, draws = 0;
    for (int tick = 0; tick <= 12; ++tick) {
        const double t = tick * 0.25;
        presents += 15;
        intervals += 15;
        sum += 15 * 16666667ull;
        draws += 15 * 19;
        uint64_t max_ns = tick == 6 ? 61000000ull : 17000000ull;
        Raw r = frame_raw(t, presents, intervals, sum, max_ns, draws);
        r.have = GTA6RT_HUD_HAVE_GPU | GTA6RT_HUD_HAVE_PIPELINES;
        r.gpu_ns = presents * 4100000ull;
        r.gpu_samples = presents;
        r.pipelines_built = 3;
        r.pipelines_pending = 0;
        m = s.update(r);
        if (tick == 7) CHECK(m.frame_max_ms > 60.0 && m.frame_max_ms < 62.0);
    }
    CHECK(std::abs(m.fps - 60.0) < 1.0);
    CHECK(std::abs(m.frame_avg_ms - 16.67) < 0.1);
    CHECK(std::abs(m.draws_pf - 19.0) < 0.01);
    CHECK(m.have_gpu && std::abs(m.gpu_ms_pf - 4.1) < 0.01);
    CHECK(m.frame_max_ms < 20.0);
    CHECK(std::abs(m.fps_smooth - 60.0) < 1.0);

    Sampler idle;
    Raw a = frame_raw(0, 100, 99, 1000000000ull, 20000000, 0), b = a;
    b.t_s = 0.25;
    idle.update(a);
    m = idle.update(b);
    CHECK(!m.have_frames && m.fps == 0);
    CHECK(m.stall_s == 0);

    Raw c = a, d = a;
    c.since_present_s = 3.2;
    d.since_present_s = 3.4;
    d.t_s = 0.25;
    Sampler stalled;
    stalled.update(c);
    m = stalled.update(d);
    CHECK(!m.have_frames && std::abs(m.stall_s - 3.4) < 1e-9);
    FormatOptions fo;
    const std::vector<Line> lines = format_lines(m, fo);
    CHECK(lines[0].text.rfind("stalled 3.4s", 0) == 0 && !lines[0].spans.empty() && lines[0].spans[0].colour == Colour::red);

    Raw e = a, f = a;
    e.presents = f.presents = 0;
    f.t_s = 0.25;
    f.since_present_s = 9.0;
    Sampler never;
    never.update(e);
    m = never.update(f);
    CHECK(m.stall_s == 0 && format_lines(m, fo)[0].text.rfind("idle", 0) == 0);
}

static void test_format() {
    Metrics m;
    m.have_frames = true;
    m.fps = 42;
    m.fps_smooth = 41.7;
    m.frame_avg_ms = 23.8;
    m.frame_max_ms = 61;
    m.draws_pf = 19;
    m.have_gpu = true;
    m.gpu_ms_pf = 4.1;
    m.have_pipelines = true;
    m.pipelines_built = 3;
    m.pipelines_pending = 0;
    m.footprint_gib = 6.2;
    m.ram_gib = 24;
    FormatOptions opt;
    opt.mode = GTA6RT_HUD_MIN;
    auto lines = format_lines(m, opt);
    CHECK(lines.size() == 1);
    CHECK(lines[0].text == "42 fps 23.8ms (max 61) | draws 19 | gpu 4.1ms | pso 3/0 pending | mem 6.2/24G");
    CHECK(lines[0].spans.empty());

    m.frame_avg_ms = 40;
    lines = format_lines(m, opt);
    CHECK(lines[0].spans.size() == 1 && lines[0].spans[0].colour == Colour::amber);
    m.frame_avg_ms = 60;
    lines = format_lines(m, opt);
    CHECK(lines[0].spans.size() == 1 && lines[0].spans[0].colour == Colour::red);
    m.frame_avg_ms = 23.8;
    m.frame_max_ms = 90;
    lines = format_lines(m, opt);
    CHECK(lines[0].spans.size() == 1 && lines[0].spans[0].colour == Colour::amber);
    m.frame_max_ms = 61;
    m.frame_max_ms = 40;
    m.pressure = 1;
    lines = format_lines(m, opt);
    CHECK(lines[0].spans.size() == 1 && lines[0].spans[0].colour == Colour::amber);
    CHECK(lines[0].text.substr(lines[0].spans[0].begin, lines[0].spans[0].length) == "mem 6.2/24G");
    m.pressure = 2;
    lines = format_lines(m, opt);
    CHECK(lines[0].spans[0].colour == Colour::red);
    m.pressure = 0;

    m.have_memory = true;
    m.budget_gib = 14;
    m.gpu_gib = 2.9;
    m.gpu_limit_gib = 17.8;
    lines = format_lines(m, opt);
    CHECK(lines[0].text.find("mem 6.2/14G gpu 2.9G") != std::string::npos);
    CHECK(lines[0].spans.empty());
    m.footprint_gib = 12.5;
    lines = format_lines(m, opt);
    CHECK(lines[0].spans.size() == 1 && lines[0].spans[0].colour == Colour::amber);
    m.mem_warning = true;
    lines = format_lines(m, opt);
    CHECK(lines[0].spans.size() == 1 && lines[0].spans[0].colour == Colour::red);
    CHECK(lines[0].text.find("gpu 2.9G LOW") != std::string::npos);
    m.mem_warning = false;
    m.footprint_gib = 6.2;
    m.have_memory = false;

    m.have_gpu = false;
    m.have_pipelines = false;
    lines = format_lines(m, opt);
    CHECK(lines[0].text.find("gpu -") != std::string::npos && lines[0].text.find("pso -") != std::string::npos);

    opt.mode = GTA6RT_HUD_FULL;
    m.have_submissions = m.have_exec = m.have_dispatches = true;
    m.submissions_pf = 2;
    m.dispatches_pf = 3;
    m.exec_ms_pf = 1.9;
    m.driver = "MoltenVK";
    m.profile = "series-x";
    m.state = "compiling 132 pipelines";
    lines = format_lines(m, opt);
    CHECK(lines.size() == 3);
    for (const auto& l : lines) CHECK(l.text.size() <= 100);
    CHECK(lines[2].text.find("MoltenVK series-x | compiling 132 pipelines") != std::string::npos);

    const std::string title = format_title("Americas", m, opt);
    CHECK(title.rfind("Americas - 42 fps", 0) == 0);
    CHECK(format_title("", m, opt).rfind("42 fps", 0) == 0);

    for (const auto& l : lines) std::printf("  | %s\n", l.text.c_str());
}

static void test_state_derivation() {
    Sampler s;
    Raw a = frame_raw(0, 0, 0, 0, 0, 0);
    a.have = GTA6RT_HUD_HAVE_PIPELINES;
    a.pipelines_pending = 132;
    s.update(a);
    a.t_s = 0.25;
    Metrics m = s.update(a);
    CHECK(m.state == "compiling 132 pipelines");
    a.state = "loading";
    a.t_s = 0.5;
    m = s.update(a);
    CHECK(m.state == "loading");
}

static void test_modes() {
    gta6rt_hud_mode m;
    CHECK(gta6rt_hud_parse_mode("off", &m) && m == GTA6RT_HUD_OFF);
    CHECK(gta6rt_hud_parse_mode("MIN", &m) && m == GTA6RT_HUD_MIN);
    CHECK(gta6rt_hud_parse_mode("minimal", &m) && m == GTA6RT_HUD_MIN);
    CHECK(gta6rt_hud_parse_mode("full", &m) && m == GTA6RT_HUD_FULL);
    CHECK(gta6rt_hud_parse_mode("Title", &m) && m == GTA6RT_HUD_TITLE);
    CHECK(!gta6rt_hud_parse_mode("loud", &m));
    CHECK(!gta6rt_hud_parse_mode(nullptr, &m));
}

static void test_keys_without_window() {
    gta6rt_hud_set_mode(GTA6RT_HUD_OFF);
    CHECK(gta6rt_hud_handle_key(0x00, 0, 0) == 0);
    CHECK(gta6rt_hud_handle_key(GTA6RT_HUD_KEY_F1, 0, 0) == 1);
    CHECK(gta6rt_hud_get_mode() == GTA6RT_HUD_MIN);
    CHECK(gta6rt_hud_handle_key(GTA6RT_HUD_KEY_F1, 0, 1) == 1);
    CHECK(gta6rt_hud_get_mode() == GTA6RT_HUD_MIN);
    gta6rt_hud_handle_key(GTA6RT_HUD_KEY_F1, 0, 0);
    CHECK(gta6rt_hud_get_mode() == GTA6RT_HUD_FULL);
    gta6rt_hud_handle_key(GTA6RT_HUD_KEY_F1, 0, 0);
    CHECK(gta6rt_hud_get_mode() == GTA6RT_HUD_TITLE);
    gta6rt_hud_handle_key(GTA6RT_HUD_KEY_F1, 0, 0);
    CHECK(gta6rt_hud_get_mode() == GTA6RT_HUD_OFF);
    gta6rt_hud_handle_key(GTA6RT_HUD_KEY_F2, 0, 0);
    CHECK(gta6rt_hud_get_mode() == GTA6RT_HUD_TITLE);
    gta6rt_hud_handle_key(GTA6RT_HUD_KEY_F2, 0, 0);
    CHECK(gta6rt_hud_get_mode() == GTA6RT_HUD_OFF);
    CHECK(gta6rt_hud_handle_key(GTA6RT_HUD_KEY_F1, 1ul << 20, 0) == 0);
    gta6rt_hud_shutdown();
    CHECK(!gta6rt_hud_active());
}

static void test_pad_collision() {
#ifdef HAVE_PAD_HEADER
    constexpr int kHidF1 = 0x3A, kHidF2 = 0x3B, kHidF11 = 0x44;
    bool clash = false;
    for (int k : rt::gameinput_mac::kKeyboardPadKeys)
        if (k == kHidF1 || k == kHidF2 || k == kHidF11) clash = true;
    CHECK(!clash);
    rt::gameinput_mac::KeySet keys;
    keys.set(kHidF1);
    keys.set(kHidF2);
    const rt::gameinput_mac::PadState state = rt::gameinput_mac::pad_state_from_keys(keys);
    CHECK(state == rt::gameinput_mac::PadState{});
#else
    ++g_checks;
#endif
}

static void bench_producers() {
    gta6rt_hud_stats* b = gta6rt_hud_stats_block();
    std::memset(b, 0, sizeof *b);
    constexpr int kN = 20000000;
    for (int active = 0; active < 2; ++active) {
        gta6rt_hud_set_active(active);
        const uint64_t t0 = gta6rt_hud_now_ns();
        for (int i = 0; i < kN; ++i) gta6rt_hud_add_draws(1);
        const uint64_t t1 = gta6rt_hud_now_ns();
        for (int i = 0; i < kN / 10; ++i) gta6rt_hud_note_present();
        const uint64_t t2 = gta6rt_hud_now_ns();
        for (int i = 0; i < kN / 10; ++i) gta6rt_hud_note_submission(100);
        const uint64_t t3 = gta6rt_hud_now_ns();
        std::printf("producer cost, %s: add_draws %.2f ns, note_present %.2f ns, note_submission %.2f ns\n", active ? "active" : "inactive",
                    double(t1 - t0) / kN, double(t2 - t1) / (kN / 10), double(t3 - t2) / (kN / 10));
    }
    gta6rt_hud_set_active(0);
}

static void test_no_window() {
    gta6rt_hud_config c{};
    c.mode = GTA6RT_HUD_FULL;
    gta6rt_hud_init(&c);
    CHECK(gta6rt_hud_active());
    CHECK(gta6rt_hud_attach(nullptr) == 0);
    gta6rt_hud_info info;
    gta6rt_hud_get_info(&info);
    CHECK(info.disabled == 1 && !info.timer_running && !info.attached);
    CHECK(!gta6rt_hud_active());
    gta6rt_hud_shutdown();
}

int main() {
    test_inactive_is_noop();
    test_c_and_cpp_share_block();
    test_activation_clears_stale();
    test_threads();
    test_present_interval();
    test_strings();
    test_ms_abi();
    test_sampler();
    test_format();
    test_state_derivation();
    test_modes();
    test_keys_without_window();
    test_pad_collision();
    bench_producers();
    test_no_window();
    std::printf("hud_unit_test: %d checks, %d failed: %s\n", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
