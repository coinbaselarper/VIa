#include "test_util.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>
#include <sstream>
#include <sys/time.h>
#include <unistd.h>

using namespace gta6rt::gpu;

namespace {

constexpr std::uint32_t kRtSize = 2048, kOut = 64, kHeavyDraws = 6;
constexpr std::uint32_t kImageSlot = 3, kSamplerSlot = 1;
constexpr std::uint32_t kRenderTarget = 0x4, kCopyDest = 0x400, kCopySource = 0x800, kShaderResource = 0x40 | 0x80;

struct Options {
    int iterations = 400;
    int inject = -1;
    bool inject_victim = false;
    std::uint32_t heavy = 4000;
    std::set<std::string> only;
    bool wants(const char* name) const { return only.empty() || only.count(name); }
};

struct Color {
    float r, g, b;
};

struct Env {
    Device* dev = nullptr;
    Queue* q = nullptr;
    std::unique_ptr<ShaderLibrary> lib;
    std::unique_ptr<RootLayout> root;
    std::unique_ptr<RenderPipeline> heavy, sample, heap;
    std::unique_ptr<Sampler> sampler;
    std::unique_ptr<DescriptorHeap> images, samplers;
    Options opt;
};

std::uint32_t fbits(float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

void set_rect(std::uint32_t* u, float x0, float y0, float x1, float y1) {
    u[8] = fbits(x0);
    u[9] = fbits(y0);
    u[10] = fbits(x1);
    u[11] = fbits(y1);
}

Color color_for(int i) { return {(i % 250) / 255.0f, ((i * 7) % 250) / 255.0f, ((i * 13) % 250) / 255.0f}; }

bool near(const std::uint8_t* p, float r, float g, float b, float a = 1.0f) {
    const float want[4] = {r, g, b, a};
    for (int k = 0; k < 4; ++k)
        if (std::abs(int(p[k]) - int(want[k] * 255.0f + 0.5f)) > 1) return false;
    return true;
}

void barrier(CommandList& l, Resource* r, std::uint32_t before, std::uint32_t after) {
    Barrier b;
    b.resource = r;
    b.before = before;
    b.after = after;
    l.barrier(std::span<const Barrier>(&b, 1));
}

void targets(CommandList& l, Texture* t) {
    TargetView v{t, 0, 0, 0};
    l.set_render_targets(std::span<const TargetView>(&v, 1), nullptr);
}

void draw_heavy(Env& e, CommandList& l, Texture* target, const Color& c, std::uint32_t loops, float x0 = -1,
                float y0 = -1, float x1 = 1, float y1 = 1) {
    targets(l, target);
    l.set_root_layout(e.root.get());
    l.set_pipeline(e.heavy.get());
    l.set_topology(topology::triangle_list);
    std::uint32_t u[16] = {};
    u[0] = fbits(c.r);
    u[1] = fbits(c.g);
    u[2] = fbits(c.b);
    u[4] = loops;
    set_rect(u, x0, y0, x1, y1);
    l.set_user_data(0, u);
    l.draw(6, 1, 0, 0);
}

void draw_sample(Env& e, CommandList& l, Texture* target) {
    targets(l, target);
    l.set_root_layout(e.root.get());
    l.set_pipeline(e.sample.get());
    l.set_topology(topology::triangle_list);
    std::uint32_t u[16] = {};
    u[0] = kImageSlot;
    u[1] = kSamplerSlot;
    set_rect(u, -1, -1, 1, 1);
    l.set_user_data(0, u);
    l.set_descriptor_table(0, e.images.get());
    l.set_descriptor_table(1, e.samplers.get());
    l.draw(6, 1, 0, 0);
}

void draw_heap(Env& e, CommandList& l, Texture* target, std::uint64_t heap_base, std::uint32_t offset) {
    targets(l, target);
    TargetView v{target, 0, 0, 0};
    l.clear_render_target(v, {0, 0, 0, 0});
    l.set_root_layout(e.root.get());
    l.set_pipeline(e.heap.get());
    l.set_topology(topology::triangle_list);
    std::uint32_t u[16] = {};
    u[5] = offset;
    set_rect(u, -1, -1, 1, 1);
    l.set_user_data(0, u);
    l.set_heap_base(heap_base);
    l.draw(6, 1, 0, 0);
}

std::unique_ptr<Texture> make_rt(Env& e, std::uint32_t size, Heap* heap, const char* label) {
    TextureDesc td;
    td.format = format::r8g8b8a8_unorm;
    td.width = td.height = size;
    td.usage = usage::render_target | usage::shader_read;
    td.label = label;
    return heap ? e.dev->create_placed_texture(heap, 0, td) : e.dev->create_texture(td);
}

std::unique_ptr<Heap> make_heap(Env& e, std::uint64_t size, MemoryKind memory, bool arena, const char* label) {
    HeapDesc hd;
    hd.size = size;
    hd.memory = memory;
    hd.console_arena = arena;
    hd.label = label;
    return e.dev->create_heap(hd);
}

bool run(Env& e, CommandList& l) {
    if (l.close() != Status::ok) return false;
    CommandList* ls[] = {&l};
    return e.q->execute(ls) == Status::ok;
}

double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

std::string wall_clock() {
    timeval tv;
    gettimeofday(&tv, nullptr);
    std::tm tm;
    localtime_r(&tv.tv_sec, &tm);
    char b[32];
    std::snprintf(b, sizeof b, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, int(tv.tv_usec / 1000));
    return b;
}

std::uint64_t cb_errors(Env& e) { return e.dev->stats().command_buffer_errors; }

struct ErrorCount {
    std::uint64_t errors = 0, victims = 0;
};

ErrorCount errors_now(Env& e) {
    const DeviceStats s = e.dev->stats();
    return {s.command_buffer_errors, s.command_buffer_victims};
}

// An iteration whose only errors are command buffers Metal discarded while recovering from another process's GPU
// error (kIOGPUCommandBufferCallbackErrorInnocentVictim) is run once more: that loss is reported (wait_idle returns
// device_lost) and says nothing about the hazard or residency model. Any other error fails the iteration.
struct Attempt {
    bool good = false;
    std::uint64_t errors = 0, victims = 0;
};

bool only_victims(const Attempt& a) { return a.errors > 0 && a.victims == a.errors; }

struct Census {
    std::uint64_t want = 0, prev = 0, zero = 0, untouched = 0, other = 0;
    std::int64_t other_at = -1;
    std::uint8_t other_px[4] = {};
};

Census census(const std::vector<std::uint8_t>& px, const Color& want, const Color& prev) {
    Census c;
    for (std::size_t i = 0; i + 4 <= px.size(); i += 4) {
        const std::uint8_t* p = &px[i];
        if (near(p, want.r, want.g, want.b)) {
            ++c.want;
        } else if (near(p, prev.r, prev.g, prev.b)) {
            ++c.prev;
        } else if (!p[0] && !p[1] && !p[2] && !p[3]) {
            ++c.zero;
        } else if (p[0] == test::kReadbackFill && p[1] == test::kReadbackFill && p[2] == test::kReadbackFill &&
                   p[3] == test::kReadbackFill) {
            ++c.untouched;
        } else {
            if (c.other_at < 0) {
                c.other_at = std::int64_t(i / 4);
                std::memcpy(c.other_px, p, 4);
            }
            ++c.other;
        }
    }
    return c;
}

std::string describe(bool ok, Status s, const std::vector<std::uint8_t>& px, const Color& want, const Color& prev) {
    char b[256];
    if (!ok) {
        std::snprintf(b, sizeof b, "read failed (%s)", status_name(s));
        return b;
    }
    const Census c = census(px, want, prev);
    std::snprintf(b, sizeof b, "%llu want, %llu previous colour, %llu zero, %llu not written by the copy, %llu other",
                  (unsigned long long)c.want, (unsigned long long)c.prev, (unsigned long long)c.zero,
                  (unsigned long long)c.untouched, (unsigned long long)c.other);
    std::string out = b;
    if (c.other_at >= 0) {
        std::snprintf(b, sizeof b, " (first other at texel %lld: %u %u %u %u)", (long long)c.other_at, c.other_px[0],
                      c.other_px[1], c.other_px[2], c.other_px[3]);
        out += b;
    }
    return out;
}

// B1: a render target written by one pass and sampled through a descriptor table by the next.
void render_then_sample(Env& e, const char* name, bool placed, bool two_executes, bool gap) {
    std::unique_ptr<Heap> heap;
    if (placed) heap = make_heap(e, 64ull << 20, MemoryKind::device_local, false, "R heap");
    auto R = make_rt(e, kRtSize, heap.get(), "R");
    auto T = make_rt(e, kOut, nullptr, "T");
    auto G = make_rt(e, kOut, nullptr, "gap");
    CHECK(R && T && G, "%s: textures", name);
    if (!R || !T || !G) return;
    e.images->write_texture(kImageSlot, R.get());
    const auto before = e.dev->stats();
    const auto t0 = std::chrono::steady_clock::now();
    const std::uint64_t texels = std::uint64_t(kOut) * kOut;
    double last_ms[4] = {};
    int bad = 0, read_failures = 0, reported = 0, repeated = 0;
    const auto attempt = [&](int i, bool inject) {
        const Color c = color_for(i), prev = color_for(i > 0 ? i - 1 : 0);
        const ErrorCount errors_before = errors_now(e);
        const auto ti = std::chrono::steady_clock::now();
        if (inject) gta6rt::metal::inject_command_buffer_failure(e.q, e.opt.inject_victim);
        auto l1 = e.dev->create_command_list(QueueType::direct);
        for (std::uint32_t k = 0; k < kHeavyDraws; ++k) draw_heavy(e, *l1, R.get(), c, e.opt.heavy);
        if (gap) draw_heavy(e, *l1, G.get(), {0, 0, 0}, 1);
        std::unique_ptr<CommandList> l2;
        CommandList* reader = l1.get();
        if (two_executes) {
            CHECK(run(e, *l1), "%s: execute 1", name);
            l2 = e.dev->create_command_list(QueueType::direct);
            reader = l2.get();
        } else {
            barrier(*l1, R.get(), kRenderTarget, kShaderResource);
        }
        draw_sample(e, *reader, T.get());
        CHECK(run(e, *reader), "%s: execute", name);
        std::vector<std::uint8_t> px;
        Status read_status = Status::ok;
        const bool read_ok = test::read_texture(*e.dev, *e.q, *T, 4, px, &read_status);
        const double ms = seconds_since(ti) * 1e3;
        const bool good = read_ok && census(px, c, prev).want == texels;
        const ErrorCount errors_at_read = errors_now(e);
        if (!good && reported++ < 3) {
            const std::string when = wall_clock();
            const Status idle = e.q->wait_idle(10'000'000'000ull);
            std::vector<std::uint8_t> again, rpx, resampled;
            Status again_status = Status::ok, r_status = Status::ok, re_status = Status::ok;
            const bool again_ok = test::read_texture(*e.dev, *e.q, *T, 4, again, &again_status);
            const bool r_ok = test::read_texture(*e.dev, *e.q, *R, 4, rpx, &r_status);
            auto l3 = e.dev->create_command_list(QueueType::direct);
            draw_sample(e, *l3, T.get());
            const bool re_run = run(e, *l3);
            const bool re_ok = test::read_texture(*e.dev, *e.q, *T, 4, resampled, &re_status);
            std::printf("  %s iteration %d%s at %s (%.1f ms; previous %.1f %.1f %.1f %.1f ms), want %.0f %.0f %.0f:\n"
                        "    command buffer errors: %llu before the iteration, %llu at the read (%llu of them discarded "
                        "by GPU recovery), %llu now; wait_idle after the read: %s\n"
                        "    T first read:  %s\n    T read again:  %s\n    R (%u x %u):   %s\n"
                        "    T sampled again from R (execute %s): %s\n",
                        name, i, inject ? " (failure injected)" : "", when.c_str(), ms, last_ms[0], last_ms[1],
                        last_ms[2], last_ms[3], c.r * 255, c.g * 255, c.b * 255,
                        (unsigned long long)errors_before.errors, (unsigned long long)errors_at_read.errors,
                        (unsigned long long)(errors_at_read.victims - errors_before.victims),
                        (unsigned long long)cb_errors(e), status_name(idle),
                        describe(read_ok, read_status, px, c, prev).c_str(),
                        describe(again_ok, again_status, again, c, prev).c_str(), kRtSize, kRtSize,
                        describe(r_ok, r_status, rpx, c, prev).c_str(), re_run ? "ok" : "failed",
                        describe(re_ok, re_status, resampled, c, prev).c_str());
        }
        if (!good) read_failures += !read_ok;
        for (int k = 3; k > 0; --k) last_ms[k] = last_ms[k - 1];
        last_ms[0] = ms;
        return Attempt{good, errors_at_read.errors - errors_before.errors,
                       errors_at_read.victims - errors_before.victims};
    };
    for (int i = 0; i < e.opt.iterations; ++i) {
        Attempt r = attempt(i, i == e.opt.inject);
        if (!r.good && only_victims(r)) {
            ++repeated;
            r = attempt(i, false);
        }
        bad += !r.good;
    }
    const auto after = e.dev->stats();
    std::printf("%s: %d of %d iterations read a wrong target (%d reads failed; %d repeated after Metal discarded them to "
                "recover from another process's GPU error; %.1f s; barriers fenced %llu, joins %llu, command buffer "
                "errors %llu)\n",
                name, bad, e.opt.iterations, read_failures, repeated, seconds_since(t0),
                (unsigned long long)(after.barriers_fenced - before.barriers_fenced),
                (unsigned long long)(after.fence_joins - before.fence_joins),
                (unsigned long long)(after.command_buffer_errors - before.command_buffer_errors));
    CHECK(bad == 0, "%s: render target sampled through a table after the write", name);
    e.images->clear(kImageSlot);
}

// A placed (untracked) render target drawn by a later pass without a barrier: D3D12 orders render-target writes.
void placed_target_order(Env& e) {
    auto heap = make_heap(e, 64ull << 20, MemoryKind::device_local, false, "R heap");
    auto R = make_rt(e, kRtSize, heap.get(), "placed R");
    auto T = make_rt(e, kOut, nullptr, "T");
    BufferDesc bd{512, MemoryKind::readback, usage::copy, "pixels"};
    auto rb = e.dev->create_buffer(bd);
    CHECK(R && T && rb, "placed_target_order: resources");
    if (!R || !T || !rb) return;
    const auto before = e.dev->stats();
    const auto t0 = std::chrono::steady_clock::now();
    int bad = 0, reported = 0, repeated = 0, idle_errors = 0;
    const auto attempt = [&](int i) {
        const Color c = color_for(i), prev = color_for(i > 0 ? i - 1 : 0);
        const ErrorCount errors_before = errors_now(e);
        const auto ti = std::chrono::steady_clock::now();
        auto l = e.dev->create_command_list(QueueType::direct);
        for (std::uint32_t k = 0; k < kHeavyDraws; ++k) draw_heavy(e, *l, R.get(), c, e.opt.heavy);
        draw_heavy(e, *l, T.get(), {0, 0, 0}, 1);
        draw_heavy(e, *l, R.get(), {1, 1, 1}, 1, -1.0f, -1.0f, -0.9f, 1.0f);
        barrier(*l, R.get(), kRenderTarget, kCopySource);
        const std::uint32_t probe[3][2] = {{kRtSize / 2, kRtSize / 2}, {120, kRtSize / 2}, {2, kRtSize / 2}};
        for (std::uint32_t k = 0; k < 3; ++k) {
            BufferTextureCopy cp;
            cp.buffer = rb.get();
            cp.offset = 128 * k;
            cp.texture = R.get();
            cp.row_pitch = 4;
            cp.width = cp.height = 1;
            cp.x = probe[k][0];
            cp.y = probe[k][1];
            l->copy_texture_to_buffer(cp);
        }
        std::memset(rb->cpu_pointer(), test::kReadbackFill, 512);
        CHECK(run(e, *l), "placed_target_order: execute");
        const Status idle = e.q->wait_idle(10'000'000'000ull);
        const ErrorCount errors_at_read = errors_now(e);
        const auto* p = static_cast<const std::uint8_t*>(rb->cpu_pointer());
        const bool good = idle == Status::ok && near(p, c.r, c.g, c.b) && near(p + 128, c.r, c.g, c.b) &&
                          near(p + 256, 1, 1, 1);
        if (!good && reported++ < 3) {
            const std::string when = wall_clock();
            const std::uint8_t first[9] = {p[0], p[1], p[2], p[128], p[129], p[130], p[256], p[257], p[258]};
            const Status idle2 = e.q->wait_idle(10'000'000'000ull);
            std::vector<std::uint8_t> rpx;
            Status r_status = Status::ok;
            const bool r_ok = test::read_texture(*e.dev, *e.q, *R, 4, rpx, &r_status);
            std::printf("  placed_target_order iteration %d at %s (%.1f ms): centre %u %u %u, next to the strip "
                        "%u %u %u (want %.0f %.0f %.0f), strip %u %u %u (238 = not written by the copy)\n"
                        "    command buffer errors: %llu before the iteration, %llu at the read (%llu of them discarded "
                        "by GPU recovery), %llu now; wait_idle: %s, again %s\n"
                        "    buffer read again: %u %u %u / %u %u %u / %u %u %u\n    R (%u x %u): %s\n",
                        i, when.c_str(), seconds_since(ti) * 1e3, first[0], first[1], first[2], first[3], first[4],
                        first[5], c.r * 255, c.g * 255, c.b * 255, first[6], first[7], first[8],
                        (unsigned long long)errors_before.errors, (unsigned long long)errors_at_read.errors,
                        (unsigned long long)(errors_at_read.victims - errors_before.victims),
                        (unsigned long long)cb_errors(e), status_name(idle), status_name(idle2), p[0], p[1], p[2],
                        p[128], p[129], p[130], p[256], p[257], p[258], kRtSize, kRtSize,
                        describe(r_ok, r_status, rpx, c, prev).c_str());
        }
        idle_errors += idle != Status::ok;
        return Attempt{good, errors_at_read.errors - errors_before.errors,
                       errors_at_read.victims - errors_before.victims};
    };
    for (int i = 0; i < e.opt.iterations; ++i) {
        Attempt r = attempt(i);
        if (!r.good && only_victims(r)) {
            ++repeated;
            r = attempt(i);
        }
        bad += !r.good;
    }
    const auto after = e.dev->stats();
    std::printf("placed_target_order: %d of %d iterations lost the first pass (wait_idle not ok %d times; %d repeated "
                "after Metal discarded them to recover from another process's GPU error; %.1f s; render-target joins "
                "%llu, command buffer errors %llu)\n",
                bad, e.opt.iterations, idle_errors, repeated, seconds_since(t0),
                (unsigned long long)(after.target_joins - before.target_joins),
                (unsigned long long)(after.command_buffer_errors - before.command_buffer_errors));
    CHECK(bad == 0, "placed render target: later pass loads what the earlier pass stored");
}

// B2: memory reached only through heap_base (arena) or through a raw buffer address must be resident.
void heap_base_read(Env& e) {
    auto T = make_rt(e, kOut, nullptr, "T");
    const std::uint64_t errors_before = cb_errors(e);
    int repeated = 0;
    const auto check = [&](const char* what, const float* want, const auto& record) {
        Attempt r;
        std::vector<std::uint8_t> px;
        for (int k = 0; k < 2; ++k) {
            const ErrorCount e0 = errors_now(e);
            auto l = e.dev->create_command_list(QueueType::direct);
            record(*l);
            Status s = Status::ok;
            const bool ok = run(e, *l) && test::read_texture(*e.dev, *e.q, *T, 4, px, &s);
            if (px.empty()) px.assign(4, test::kReadbackFill);
            const ErrorCount e1 = errors_now(e);
            r = Attempt{ok && near(px.data(), want[0], want[1], want[2], want[3]), e1.errors - e0.errors,
                        e1.victims - e0.victims};
            if (r.good || !only_victims(r)) break;
            ++repeated;
        }
        std::printf("heap_base: %s read %u %u %u %u, want %.0f %.0f %.0f %.0f\n", what, px[0], px[1], px[2], px[3],
                    want[0] * 255, want[1] * 255, want[2] * 255, want[3] * 255);
        CHECK(r.good, "pixel shader reads %s through heap_base", what);
    };

    auto arena = make_heap(e, 16ull << 20, MemoryKind::shared, true, "shared arena");
    CHECK(arena && arena->arena(), "shared arena");
    if (!arena || !arena->arena()) return;
    const float a[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    std::memcpy(static_cast<std::uint8_t*>(arena->arena()->cpu_pointer()) + 0x1000, a, 16);
    check("shared arena + 0x1000 (in no table, never bound)", a,
          [&](CommandList& l) { draw_heap(e, l, T.get(), arena->host_gpu_address(), 0x1000); });

    auto local = make_heap(e, 16ull << 20, MemoryKind::device_local, true, "device-local arena");
    BufferDesc ud{16, MemoryKind::upload, usage::copy, "arena upload"};
    auto up = e.dev->create_buffer(ud);
    CHECK(local && local->arena() && up, "device-local arena");
    if (!local || !local->arena() || !up) return;
    const float b[4] = {0.75f, 0.25f, 0.5f, 1.0f};
    std::memcpy(up->cpu_pointer(), b, 16);
    check("device-local arena + 0x2000 (written by a copy in an earlier execute)", b, [&](CommandList& l) {
        auto u = e.dev->create_command_list(QueueType::direct);
        u->copy_buffer(local->arena(), 0x2000, up.get(), 0, 16);
        CHECK(run(e, *u), "heap_base: upload");
        e.q->wait_idle(10'000'000'000ull);
        barrier(l, local->arena(), kCopyDest, kShaderResource);
        draw_heap(e, l, T.get(), local->host_gpu_address(), 0x2000);
    });

    BufferDesc cd{4096, MemoryKind::shared, usage::constant, "committed, by address only"};
    auto cbuf = e.dev->create_buffer(cd);
    const float c[4] = {0.125f, 0.25f, 0.375f, 0.5f};
    std::memcpy(static_cast<std::uint8_t*>(cbuf->cpu_pointer()) + 0x40, c, 16);
    check("committed buffer + 0x40 by address", c,
          [&](CommandList& l) { draw_heap(e, l, T.get(), cbuf->host_gpu_address(), 0x40); });
    std::printf("heap_base: command buffer errors %llu, %d reads repeated after Metal discarded them to recover from "
                "another process's GPU error\n",
                (unsigned long long)(cb_errors(e) - errors_before), repeated);
}

// B3: draws before a value wait (an earlier command buffer of the same execute) keep their residency.
void wait_split(Env& e, bool with_wait) {
    const char* name = with_wait ? "wait_split" : "bindless";
    auto R = make_rt(e, 256, nullptr, "R");
    auto T = make_rt(e, kOut, nullptr, "T before");
    auto T2 = make_rt(e, kOut, nullptr, "T after");
    BufferDesc vd{4096, MemoryKind::shared, usage::copy, "values"};
    auto vals = e.dev->create_buffer(vd);
    std::memset(vals->cpu_pointer(), 0, 4096);
    *static_cast<std::uint64_t*>(vals->cpu_pointer()) = 9;
    e.images->write_texture(kImageSlot, R.get());
    const int n = std::max(1, std::min(e.opt.iterations, 10));
    int bad = 0, reported = 0, repeated = 0;
    const auto before = e.dev->stats();
    const auto attempt = [&](int i) {
        const Color c = color_for(i + 50);
        const ErrorCount errors_before = errors_now(e);
        auto l = e.dev->create_command_list(QueueType::direct);
        TargetView rv{R.get(), 0, 0, 0};
        l->clear_render_target(rv, {0, 0, 0, 1});
        draw_heavy(e, *l, R.get(), c, e.opt.heavy / 10);
        barrier(*l, R.get(), kRenderTarget, kShaderResource);
        draw_sample(e, *l, T.get());
        if (with_wait) l->wait_value(vals.get(), 0, 5, 8, WaitCompare::greater_equal);
        draw_sample(e, *l, T2.get());
        CHECK(run(e, *l), "%s: execute", name);
        const Status idle = e.q->wait_idle(10'000'000'000ull);
        std::vector<std::uint8_t> a, b;
        Status sa = Status::ok, sb = Status::ok;
        const bool ok_a = test::read_texture(*e.dev, *e.q, *T, 4, a, &sa);
        const bool ok_b = test::read_texture(*e.dev, *e.q, *T2, 4, b, &sb);
        const ErrorCount errors_after = errors_now(e);
        const Color prev = color_for(i > 0 ? i + 49 : 50);
        const bool good = idle == Status::ok && ok_a && ok_b && census(a, c, prev).want == a.size() / 4 &&
                          census(b, c, prev).want == b.size() / 4;
        if (!good && reported++ < 3)
            std::printf("  %s iteration %d at %s, want %.0f %.0f %.0f: wait_idle %s; before: %s; after: %s; command "
                        "buffer errors %llu before, %llu now (%llu discarded by GPU recovery)\n",
                        name, i, wall_clock().c_str(), c.r * 255, c.g * 255, c.b * 255, status_name(idle),
                        describe(ok_a, sa, a, c, prev).c_str(), describe(ok_b, sb, b, c, prev).c_str(),
                        (unsigned long long)errors_before.errors, (unsigned long long)errors_after.errors,
                        (unsigned long long)(errors_after.victims - errors_before.victims));
        return Attempt{good, errors_after.errors - errors_before.errors, errors_after.victims - errors_before.victims};
    };
    for (int i = 0; i < n; ++i) {
        Attempt r = attempt(i);
        if (!r.good && only_victims(r)) {
            ++repeated;
            r = attempt(i);
        }
        bad += !r.good;
    }
    const auto after = e.dev->stats();
    std::printf("%s: %d of %d lists drew a wrong colour (%d repeated after Metal discarded them to recover from another "
                "process's GPU error; command buffers %llu, value waits %llu, command buffer errors %llu)\n",
                name, bad, n, repeated, (unsigned long long)(after.command_buffers - before.command_buffers),
                (unsigned long long)(after.value_waits - before.value_waits),
                (unsigned long long)(after.command_buffer_errors - before.command_buffer_errors));
    CHECK(bad == 0, "%s: bindless draws on both sides of the wait read the render target", name);
    e.images->clear(kImageSlot);
}

// A texture written once, then copied into a new readback buffer and read by the CPU many times.
void readback_repeat(Env& e) {
    auto T = make_rt(e, kOut, nullptr, "T");
    CHECK(T != nullptr, "readback_repeat: texture");
    if (!T) return;
    const Color c = color_for(77);
    auto l = e.dev->create_command_list(QueueType::direct);
    draw_heavy(e, *l, T.get(), c, 1);
    CHECK(run(e, *l) && e.q->wait_idle(10'000'000'000ull) == Status::ok, "readback_repeat: draw");
    const int n = e.opt.iterations * 25;
    const std::uint64_t texels = std::uint64_t(kOut) * kOut;
    int bad = 0;
    const std::uint64_t errors0 = cb_errors(e);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) {
        std::vector<std::uint8_t> px;
        Status s = Status::ok;
        const bool ok = test::read_texture(*e.dev, *e.q, *T, 4, px, &s);
        if (ok && census(px, c, c).want == texels) continue;
        if (bad < 3) {
            std::vector<std::uint8_t> again;
            Status s2 = Status::ok;
            const bool ok2 = test::read_texture(*e.dev, *e.q, *T, 4, again, &s2);
            std::printf("  readback_repeat read %d at %s: %s; read again: %s; command buffer errors %llu\n", i,
                        wall_clock().c_str(), describe(ok, s, px, c, c).c_str(), describe(ok2, s2, again, c, c).c_str(),
                        (unsigned long long)(cb_errors(e) - errors0));
        }
        ++bad;
    }
    std::printf("readback_repeat: %d of %d reads wrong (%.1f s, %.3f ms a read; command buffer errors %llu)\n", bad, n,
                seconds_since(t0), seconds_since(t0) * 1e3 / n, (unsigned long long)(cb_errors(e) - errors0));
    CHECK(bad == 0, "readback_repeat: every read returns the texture");
}

// DESIGN 3.12 with a real Metal command-buffer error (inject_command_buffer_failure: InvalidResource, this process
// only). The three historical metal_hazards failures were command buffers Metal discarded while recovering from
// another process's GPU hang; such a command buffer also runs nothing and skips its event signals. (The injection does
// not fail a command buffer that samples GPU counters, measured 0 of 5, so the timestamp-resolve split is covered only
// by the same timeline wait that signal() encodes.)
void gpu_error(Env& e) {
    if (const char* v = std::getenv("MTL_DEBUG_LAYER"); v && *v && *v != '0') {
        std::printf("gpu_error: skipped (MTL_DEBUG_LAYER aborts on the released buffer the injection uses)\n");
        return;
    }
    const bool repair = !std::getenv("GTA6RT_METAL_NO_ERROR_REPAIR");
    constexpr std::uint64_t kShort = 2'000'000'000ull;
    const auto ms_since = [](std::chrono::steady_clock::time_point t) { return seconds_since(t) * 1e3; };
    auto fence = e.dev->create_fence(0);
    CHECK(fence != nullptr, "gpu_error: fence");
    if (!fence) return;
    CHECK(e.q->wait_idle(10'000'000'000ull) == Status::ok, "gpu_error: idle before");
    const DeviceStats s0 = e.dev->stats();

    if (!repair) {
        gta6rt::metal::inject_command_buffer_failure(e.q);
        CHECK(e.q->signal(fence.get(), 1) == Status::ok, "gpu_error: signal");
        auto t = std::chrono::steady_clock::now();
        const Status fw = fence->wait_cpu(1, kShort);
        const double fence_ms = ms_since(t);
        t = std::chrono::steady_clock::now();
        const Status idle = e.q->wait_idle(kShort);
        const double idle_ms = ms_since(t);
        std::printf("gpu_error without the repair (negative control): the failed command buffer's fence signal: wait "
                    "%s after %.0f ms (value %llu); wait_idle %s after %.0f ms; errors +%llu\n",
                    status_name(fw), fence_ms, (unsigned long long)fence->completed_value(), status_name(idle), idle_ms,
                    (unsigned long long)(cb_errors(e) - s0.command_buffer_errors));
        CHECK(fw == Status::timeout && idle == Status::timeout,
              "gpu_error: without the repair the skipped signals block every later wait");
        return;
    }

    auto R = make_rt(e, kRtSize, nullptr, "R");
    auto T = make_rt(e, kOut, nullptr, "T");
    CHECK(R && T, "gpu_error: textures");
    if (!R || !T) return;
    e.images->write_texture(kImageSlot, R.get());
    const auto iteration = [&](const Color& c) {
        auto l = e.dev->create_command_list(QueueType::direct);
        for (std::uint32_t k = 0; k < kHeavyDraws; ++k) draw_heavy(e, *l, R.get(), c, e.opt.heavy / 10);
        barrier(*l, R.get(), kRenderTarget, kShaderResource);
        draw_sample(e, *l, T.get());
        return run(e, *l);
    };
    const std::uint64_t texels = std::uint64_t(kOut) * kOut;
    const Color c0 = color_for(10), c1 = color_for(11);
    std::vector<std::uint8_t> px, again, rpx, retry;
    CHECK(iteration(c0) && test::read_texture(*e.dev, *e.q, *T, 4, px) && census(px, c0, c0).want == texels,
          "gpu_error: the iteration before the failure");

    const std::uint64_t e1 = cb_errors(e);
    gta6rt::metal::inject_command_buffer_failure(e.q);
    auto t = std::chrono::steady_clock::now();
    const bool executed = iteration(c1);
    Status read_status = Status::ok, again_status = Status::ok;
    const bool read_ok = test::read_texture(*e.dev, *e.q, *T, 4, px, &read_status);
    const double read_ms = ms_since(t);
    const std::uint64_t errors_at_read = cb_errors(e) - e1;
    const Status idle = e.q->wait_idle(kShort);
    const bool again_ok = test::read_texture(*e.dev, *e.q, *T, 4, again, &again_status);
    const bool r_ok = test::read_texture(*e.dev, *e.q, *R, 4, rpx);
    const bool retry_ok = iteration(c1) && test::read_texture(*e.dev, *e.q, *T, 4, retry);
    std::printf("gpu_error: same_list iteration with its command buffer failed: execute %s; first read %s after %.1f ms; "
                "command buffer errors +%llu at the read; wait_idle after it %s\n"
                "    T read again: %s\n    R: %s\n    iteration run again: %s\n",
                executed ? "ok" : "failed", describe(read_ok, read_status, px, c1, c0).c_str(), read_ms,
                (unsigned long long)errors_at_read, status_name(idle), describe(again_ok, again_status, again, c1, c0).c_str(),
                describe(r_ok, Status::ok, rpx, c1, c0).c_str(), describe(retry_ok, Status::ok, retry, c1, c0).c_str());
    CHECK(executed, "gpu_error: execute returns ok (errors are reported by wait_idle)");
    CHECK(!read_ok && read_status == Status::device_lost, "gpu_error: the next wait_idle reports device_lost");
    CHECK(read_ms < 1000, "gpu_error: device_lost is reported without waiting for the timeout (%.1f ms)", read_ms);
    CHECK(errors_at_read == 1, "gpu_error: one command buffer error counted (%llu)", (unsigned long long)errors_at_read);
    CHECK(idle == Status::ok, "gpu_error: device_lost is reported once");
    CHECK(again_ok && census(again, c1, c0).prev == texels, "gpu_error: the failed command buffer drew nothing into T");
    CHECK(r_ok && census(rpx, c1, c0).prev == rpx.size() / 4, "gpu_error: the failed command buffer drew nothing into R");
    CHECK(retry_ok && census(retry, c1, c0).want == texels, "gpu_error: the queue runs the iteration again");

    const std::uint64_t e2 = cb_errors(e);
    gta6rt::metal::inject_command_buffer_failure(e.q);
    CHECK(e.q->signal(fence.get(), 5) == Status::ok, "gpu_error: signal");
    t = std::chrono::steady_clock::now();
    const Status own = fence->wait_cpu(5, kShort);
    const double own_ms = ms_since(t);
    const Status idle2 = e.q->wait_idle(kShort);

    gta6rt::metal::inject_command_buffer_failure(e.q);
    CHECK(iteration(c0), "gpu_error: execute");
    CHECK(e.q->signal(fence.get(), 6) == Status::ok, "gpu_error: signal");
    t = std::chrono::steady_clock::now();
    const Status later = fence->wait_cpu(6, kShort);
    const double later_ms = ms_since(t);
    const Status idle3 = e.q->wait_idle(kShort);

    const DeviceStats s1 = e.dev->stats();
    std::printf("gpu_error: fence signal in the failed command buffer: wait %s after %.1f ms, wait_idle %s; signal after a "
                "failed execute (its command buffer waits for the failed one's timeline value): wait %s after %.1f ms, "
                "wait_idle %s\n"
                "gpu_error: command buffer errors +%llu (discarded by GPU recovery %llu), event signals repaired %llu\n",
                status_name(own), own_ms, status_name(idle2), status_name(later), later_ms, status_name(idle3),
                (unsigned long long)(s1.command_buffer_errors - s0.command_buffer_errors),
                (unsigned long long)(s1.command_buffer_victims - s0.command_buffer_victims),
                (unsigned long long)(s1.event_signals_repaired - s0.event_signals_repaired));
    CHECK(cb_errors(e) - e2 == 2, "gpu_error: two more errors counted");
    CHECK(own == Status::ok && own_ms < 1000, "gpu_error: a fence signalled by a failed command buffer still arrives");
    CHECK(later == Status::ok && later_ms < 1000, "gpu_error: a signal after a failed execute arrives");
    CHECK(idle2 == Status::device_lost && idle3 == Status::device_lost,
          "gpu_error: each failure is reported by the next wait_idle");
    CHECK(s1.event_signals_repaired - s0.event_signals_repaired == 4,
          "gpu_error: the CPU did the 3 skipped timeline signals and the skipped fence signal");
    CHECK(s1.command_buffer_victims == s0.command_buffer_victims, "gpu_error: an own error is not counted as a victim");
    CHECK(e.q->wait_idle(kShort) == Status::ok, "gpu_error: idle after");
    e.images->clear(kImageSlot);
}

// What shader validation does with a load outside its buffer (measured, round-2 review response): it reports the
// load on stderr, the load reads zero, and the command buffer completes without an error. So validation cannot drive
// the device_lost path of DESIGN 3.12; only a real GPU fault or timeout can. Without validation the same load would be
// a real GPU fault, so the case runs only under MTL_SHADER_VALIDATION=1.
void validation_fault(Env& e) {
    const char* v = std::getenv("MTL_SHADER_VALIDATION");
    if (!v || std::strcmp(v, "1") != 0) {
        std::printf("validation_fault: skipped (needs MTL_SHADER_VALIDATION=1; without it the load is a GPU fault)\n");
        return;
    }
    auto T = make_rt(e, kOut, nullptr, "T");
    auto arena = make_heap(e, 16ull << 20, MemoryKind::shared, true, "shared arena");
    BufferDesc sd{4096, MemoryKind::shared, usage::constant, "4 KiB buffer"};
    auto small = e.dev->create_buffer(sd);
    CHECK(T && arena && arena->arena() && small, "validation_fault: resources");
    if (!T || !arena || !arena->arena() || !small) return;
    const float a[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    std::memcpy(static_cast<std::uint8_t*>(arena->arena()->cpu_pointer()) + 0x1000, a, 16);
    std::memset(small->cpu_pointer(), 0x7f, 4096);
    CHECK(e.q->wait_idle(10'000'000'000ull) == Status::ok, "validation_fault: idle before");
    const std::uint64_t errors0 = cb_errors(e);

    auto l = e.dev->create_command_list(QueueType::direct);
    draw_heap(e, *l, T.get(), small->host_gpu_address(), 64u << 20);
    const bool executed = run(e, *l);
    const Status idle = e.q->wait_idle(10'000'000'000ull);
    std::vector<std::uint8_t> bad, good;
    Status bad_status = Status::ok, good_status = Status::ok;
    const bool bad_ok = test::read_texture(*e.dev, *e.q, *T, 4, bad, &bad_status);
    if (bad.empty()) bad.assign(4, test::kReadbackFill);
    const std::uint64_t errors1 = cb_errors(e);

    l = e.dev->create_command_list(QueueType::direct);
    draw_heap(e, *l, T.get(), arena->host_gpu_address(), 0x1000);
    const bool good_ok = run(e, *l) && test::read_texture(*e.dev, *e.q, *T, 4, good, &good_status);
    if (good.empty()) good.assign(4, test::kReadbackFill);
    std::printf("validation_fault: load 64 MiB past a 4 KiB buffer: execute %s, wait_idle %s, read %u %u %u %u (%s), "
                "command buffer errors +%llu; next draw read %u %u %u %u (%s)\n",
                executed ? "ok" : "failed", status_name(idle), bad[0], bad[1], bad[2], bad[3], status_name(bad_status),
                (unsigned long long)(errors1 - errors0), good[0], good[1], good[2], good[3], status_name(good_status));
    CHECK(executed && idle == Status::ok && bad_ok, "validation_fault: the command buffer completes without an error");
    CHECK(errors1 == errors0, "validation_fault: no command buffer error counted");
    CHECK(bad[0] == 0 && bad[1] == 0 && bad[2] == 0 && bad[3] == 0, "validation_fault: the bad load reads zero");
    CHECK(good_ok && near(good.data(), a[0], a[1], a[2], a[3]), "validation_fault: the next draw is unaffected");
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--iterations" && i + 1 < argc) {
            opt.iterations = std::atoi(argv[++i]);
        } else if (a == "--inject" && i + 1 < argc) {
            opt.inject = std::atoi(argv[++i]);
        } else if (a == "--inject-victim") {
            opt.inject_victim = true;
        } else if (a == "--heavy" && i + 1 < argc) {
            opt.heavy = std::uint32_t(std::atoi(argv[++i]));
        } else if (a == "--only" && i + 1 < argc) {
            std::stringstream s(argv[++i]);
            for (std::string t; std::getline(s, t, ',');) opt.only.insert(t);
        }
    }
    auto dev = gta6rt::metal::create_device();
    if (!dev) return 1;
    test::print_caps(dev->caps());
    for (const char* v : {"MTL_DEBUG_LAYER", "MTL_SHADER_VALIDATION", "GTA6RT_METAL_NO_RESIDENCY",
                          "GTA6RT_METAL_NO_DEVICE_RESIDENCY", "GTA6RT_METAL_NO_TARGET_JOIN", "GTA6RT_TEST_NO_READBACK_FILL",
                          "GTA6RT_METAL_NO_ERROR_REPAIR"})
        if (const char* s = std::getenv(v)) std::printf("env %s=%s\n", v, s);
    std::printf("iterations %d, heavy loop %u x %u draws, started %s, pid %d\n", opt.iterations, opt.heavy, kHeavyDraws,
                wall_clock().c_str(), int(getpid()));
    auto queue = dev->create_queue(QueueType::direct, "hazards");
    Env e;
    e.dev = dev.get();
    e.q = queue.get();
    e.opt = opt;
    std::string err;
    e.lib = dev->create_library_msl(test::read_file(test::shader_path("hazards.metal")), MslOptions{}, &err);
    CHECK(e.lib != nullptr, "MSL library: %s", err.c_str());
    if (!e.lib) return test::finish("metal_test_hazards");
    RootLayoutDesc rd;
    rd.tables = {{0, DescriptorKind::texture}, {1, DescriptorKind::sampler}};
    e.root = dev->create_root_layout(rd);
    auto make = [&](const char* ps) {
        RenderPipelineDesc pd;
        pd.vs = {e.lib.get(), "vs_rect"};
        pd.ps = {e.lib.get(), ps};
        pd.rt_formats[0] = format::r8g8b8a8_unorm;
        pd.rt_count = 1;
        pd.raster.cull = CullMode::none;
        pd.root = e.root.get();
        pd.mode = CompileMode::eager;
        auto p = dev->create_render_pipeline(pd);
        CHECK(p->state() == PipelineState::ready && p->reflection().mismatches.empty(), "pipeline %s: %s", ps,
              p->error().c_str());
        return p;
    };
    e.heavy = make("ps_heavy");
    e.sample = make("ps_sample");
    e.heap = make("ps_heap");
    SamplerDesc sd;
    sd.min = sd.mag = sd.mip = Filter::point;
    sd.u = sd.v = sd.w = AddressMode::clamp;
    e.sampler = dev->create_sampler(sd);
    e.images = dev->create_descriptor_heap({DescriptorKind::texture, 16, "images"});
    e.samplers = dev->create_descriptor_heap({DescriptorKind::sampler, 16, "samplers"});
    e.samplers->write_sampler(kSamplerSlot, e.sampler.get());
    if (test::failures) return test::finish("metal_test_hazards");

    if (opt.wants("same_list")) render_then_sample(e, "same_list (committed R, barrier)", false, false, false);
    if (opt.wants("two_executes"))
        render_then_sample(e, "two_executes (committed R, no barrier, execute boundary)", false, true, false);
    if (opt.wants("placed")) render_then_sample(e, "placed (placed R, barrier)", true, false, false);
    if (opt.wants("placed_gap"))
        render_then_sample(e, "placed_gap (placed R, other pass between write and barrier)", true, false, true);
    if (opt.wants("placed_target_order")) placed_target_order(e);
    if (opt.wants("heap_base")) heap_base_read(e);
    if (opt.wants("bindless")) wait_split(e, false);
    if (opt.wants("wait_split")) wait_split(e, true);
    if (!opt.only.empty() && opt.wants("readback_repeat")) readback_repeat(e);
    if (!opt.only.empty() && opt.wants("gpu_error")) gpu_error(e);
    if (!opt.only.empty() && opt.wants("validation_fault")) validation_fault(e);
    queue->wait_idle(std::getenv("GTA6RT_METAL_NO_ERROR_REPAIR") ? 1'000'000'000ull : 10'000'000'000ull);
    test::print_stats(dev->stats());
    return test::finish("metal_test_hazards");
}
