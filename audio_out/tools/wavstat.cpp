// audio_out_wavstat FILE [--window MS] [--csv OUT] [--from S] [--to S] [--silence DBFS]
// Level, timing and spectrum of a captured WAV: per-channel peak/RMS, RMS per window, active span, longest run of
// exact zeros inside it, and the strongest spectral peaks of the active span.
#include "wav.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace gta6rt::audio_out;

namespace {

double db(double x) { return x > 0 ? 20 * std::log10(x) : -999; }

void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * M_PI / len;
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t j = 0; j < len / 2; ++j) {
                const auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s FILE [--window MS] [--csv OUT] [--from S] [--to S] [--silence DBFS]\n", argv[0]);
        return 2;
    }
    double window_ms = 100, from = 0, to = 1e18, silence = -60;
    const char* csv = nullptr;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--window")) window_ms = std::atof(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--csv")) csv = argv[i + 1];
        else if (!std::strcmp(argv[i], "--from")) from = std::atof(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--to")) to = std::atof(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--silence")) silence = std::atof(argv[i + 1]);
    }
    WavData w;
    if (!read_wav(argv[1], &w)) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    const size_t ch = w.channels, total = w.samples.size() / ch;
    const size_t a = std::min(total, static_cast<size_t>(from * w.rate));
    const size_t b = std::min(total, static_cast<size_t>(std::min(to * w.rate, 1e15)));
    std::printf("%s: %u Hz, %zu ch, %zu frames (%.3f s); analysed %.3f..%.3f s\n", argv[1], w.rate, ch, total,
                double(total) / w.rate, double(a) / w.rate, double(b) / w.rate);
    std::vector<double> peak(ch, 0), energy(ch, 0);
    for (size_t i = a; i < b; ++i)
        for (size_t c = 0; c < ch; ++c) {
            const double x = w.samples[i * ch + c];
            peak[c] = std::max(peak[c], std::fabs(x));
            energy[c] += x * x;
        }
    for (size_t c = 0; c < ch; ++c)
        std::printf("  ch%zu: peak %.4f (%.1f dBFS), rms %.5f (%.1f dBFS)\n", c, peak[c], db(peak[c]),
                    std::sqrt(energy[c] / std::max<size_t>(1, b - a)), db(std::sqrt(energy[c] / std::max<size_t>(1, b - a))));

    const size_t win = std::max<size_t>(1, static_cast<size_t>(window_ms * w.rate / 1000));
    FILE* out = csv ? std::fopen(csv, "w") : nullptr;
    if (out) std::fprintf(out, "t_s,rms_dbfs,peak\n");
    size_t active = 0, windows = 0;
    double first_active = -1, last_active = -1;
    for (size_t at = a; at < b; at += win) {
        const size_t e = std::min(b, at + win);
        double en = 0, pk = 0;
        for (size_t i = at; i < e; ++i)
            for (size_t c = 0; c < ch; ++c) {
                const double x = w.samples[i * ch + c];
                en += x * x;
                pk = std::max(pk, std::fabs(x));
            }
        const double rms = std::sqrt(en / ((e - at) * ch));
        ++windows;
        if (db(rms) > silence) {
            ++active;
            if (first_active < 0) first_active = double(at) / w.rate;
            last_active = double(e) / w.rate;
        }
        if (out) std::fprintf(out, "%.3f,%.2f,%.5f\n", double(at) / w.rate, db(rms), pk);
    }
    if (out) std::fclose(out);
    std::printf("  windows of %.0f ms: %zu, above %.0f dBFS: %zu; active span %.3f..%.3f s\n", window_ms, windows, silence,
                active, first_active, last_active);
    if (first_active < 0) return 0;

    const size_t s0 = static_cast<size_t>(first_active * w.rate), s1 = static_cast<size_t>(last_active * w.rate);
    size_t run = 0, longest = 0, longest_at = 0, zero_runs_over_1ms = 0;
    for (size_t i = s0; i < s1 && i < total; ++i) {
        bool zero = true;
        for (size_t c = 0; c < ch; ++c) zero = zero && w.samples[i * ch + c] == 0.0f;
        if (zero) {
            if (++run > longest) longest = run, longest_at = i + 1 - run;
        } else {
            if (run > w.rate / 1000) ++zero_runs_over_1ms;
            run = 0;
        }
    }
    std::printf("  inside the active span: longest run of exact zeros %zu frames (%.1f ms) at %.3f s; zero runs > 1 ms: %zu\n",
                longest, longest * 1000.0 / w.rate, double(longest_at) / w.rate, zero_runs_over_1ms);

    std::printf("  per second (rms dBFS, all channels):");
    for (size_t at = s0; at < s1; at += w.rate) {
        const size_t e = std::min(s1, at + w.rate);
        double en = 0;
        for (size_t i = at; i < e; ++i)
            for (size_t c = 0; c < ch; ++c) en += double(w.samples[i * ch + c]) * w.samples[i * ch + c];
        std::printf(" %.1f", db(std::sqrt(en / ((e - at) * ch))));
    }
    std::printf("\n");

    constexpr size_t N = 8192;
    std::vector<double> spec(N / 2, 0);
    size_t blocks = 0;
    for (size_t at = s0; at + N <= s1; at += N) {
        std::vector<std::complex<double>> v(N);
        for (size_t i = 0; i < N; ++i) {
            double x = 0;
            for (size_t c = 0; c < ch; ++c) x += w.samples[(at + i) * ch + c];
            v[i] = x / ch * (0.5 - 0.5 * std::cos(2 * M_PI * i / (N - 1)));
        }
        fft(v);
        for (size_t k = 0; k < N / 2; ++k) spec[k] += std::norm(v[k]);
        ++blocks;
    }
    if (!blocks) return 0;
    std::vector<size_t> idx;
    for (size_t k = 2; k + 2 < N / 2; ++k)
        if (spec[k] > spec[k - 1] && spec[k] >= spec[k + 1]) idx.push_back(k);
    std::sort(idx.begin(), idx.end(), [&](size_t x, size_t y) { return spec[x] > spec[y]; });
    std::printf("  spectrum (%zu x %zu-point Hann blocks, mono sum), strongest peaks:", blocks, N);
    for (size_t i = 0; i < std::min<size_t>(8, idx.size()); ++i)
        std::printf(" %.0f Hz (%.1f dB)", double(idx[i]) * w.rate / N, 10 * std::log10(spec[idx[i]] / spec[idx[0]]));
    std::printf("\n");
    double lo = 0, mid = 0, hi = 0, all = 0;
    for (size_t k = 1; k < N / 2; ++k) {
        const double f = double(k) * w.rate / N;
        all += spec[k];
        (f < 250 ? lo : f < 4000 ? mid : hi) += spec[k];
    }
    std::printf("  energy split: <250 Hz %.1f%%, 250-4000 Hz %.1f%%, >4 kHz %.1f%%\n", 100 * lo / all, 100 * mid / all,
                100 * hi / all);
    return 0;
}
