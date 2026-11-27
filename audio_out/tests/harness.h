#pragma once

#include <gta6rt/audio_out.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include <mach/mach_time.h>

inline uint64_t now_ns() {
    static mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t;
        mach_timebase_info(&t);
        return t;
    }();
    return mach_absolute_time() * tb.numer / tb.denom;
}

// The shim's structure: a pacer thread signals the title's auto-reset event whenever padding is below target, and
// the "title" renders one 480-frame pass per wake with a jittery response time.
struct Harness {
    gta6rt_audio_stream* s = nullptr;
    uint32_t channels = 2;
    uint32_t target = 2880;
    uint32_t pass = 480;
    double jitter_prob = 0.1;
    int jitter_max_ms = 20;
    std::atomic<bool> stop{false};
    std::mutex m;
    std::condition_variable cv;
    bool signaled = false;
    std::atomic<uint64_t> passes{0}, max_write_ns{0}, frame_index{0}, wakes{0};
    std::thread pacer, producer, reader;
    std::vector<std::pair<uint64_t, uint64_t>> positions;
    std::atomic<uint64_t> position_regressions{0};
    float amp = 0.25f;

    void signal() {
        {
            std::lock_guard<std::mutex> l(m);
            signaled = true;
        }
        cv.notify_one();
    }

    static float sample(uint64_t n, uint32_t c) {
        if (c == 0) return static_cast<float>(n % 48000) / 48000.0f - 0.5f;
        return 0.25f * static_cast<float>(std::sin(2 * M_PI * (440.0 * (c + 1)) * double(n) / 48000.0));
    }

    void run_threads() {
        pacer = std::thread([this] {
            while (!stop.load()) {
                gta6rt_audio_stream_wait_progress(s, 5);
                if (gta6rt_audio_stream_padding(s) + pass <= target) signal();
            }
        });
        producer = std::thread([this] {
            std::mt19937 rng(7);
            std::uniform_real_distribution<double> u(0, 1);
            std::vector<float> buf(size_t(pass) * channels);
            while (!stop.load()) {
                {
                    std::unique_lock<std::mutex> l(m);
                    cv.wait_for(l, std::chrono::milliseconds(100), [this] { return signaled; });
                    if (!signaled) continue;
                    signaled = false;
                }
                wakes.fetch_add(1);
                const double r = u(rng);
                const double ms = r < jitter_prob ? 5 + u(rng) * (jitter_max_ms - 5) : u(rng) * 2;
                std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(ms * 1000)));
                const uint64_t base = frame_index.load();
                for (uint32_t f = 0; f < pass; ++f)
                    for (uint32_t c = 0; c < channels; ++c) buf[size_t(f) * channels + c] = sample(base + f, c);
                const uint64_t t0 = now_ns();
                const uint32_t n = gta6rt_audio_stream_write(s, buf.data(), pass);
                const uint64_t dt = now_ns() - t0;
                if (dt > max_write_ns.load()) max_write_ns.store(dt);
                frame_index.fetch_add(n);
                passes.fetch_add(1);
                if (gta6rt_audio_stream_padding(s) + pass <= target) signal();
            }
        });
        reader = std::thread([this] {
            uint64_t last = 0;
            while (!stop.load()) {
                uint64_t t = 0;
                const uint64_t p = gta6rt_audio_stream_position(s, &t);
                if (p < last) position_regressions.fetch_add(1);
                last = p;
                if (positions.size() < 400000) positions.emplace_back(t, p);
                std::this_thread::sleep_for(std::chrono::microseconds(250));
            }
        });
    }
    void join() {
        stop.store(true);
        signal();
        pacer.join();
        producer.join();
        reader.join();
    }
};

struct Fit {
    double rate = 0, max_dev = 0;
};

// Least-squares fit of position against time over the samples after `skip_ns`.
inline Fit fit_positions(const std::vector<std::pair<uint64_t, uint64_t>>& v, uint64_t from_ns) {
    double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (auto& [t, p] : v) {
        if (t < from_ns || p == 0) continue;
        const double x = (t - from_ns) * 1e-9, y = double(p);
        n += 1, sx += x, sy += y, sxx += x * x, sxy += x * y;
    }
    Fit f;
    if (n < 2) return f;
    f.rate = (n * sxy - sx * sy) / (n * sxx - sx * sx);
    const double b = (sy - f.rate * sx) / n;
    for (auto& [t, p] : v) {
        if (t < from_ns || p == 0) continue;
        f.max_dev = std::max(f.max_dev, std::fabs(double(p) - (f.rate * (t - from_ns) * 1e-9 + b)));
    }
    return f;
}
