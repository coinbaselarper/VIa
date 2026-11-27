#include "check.h"
#include "downmix.h"

#include <cmath>
#include <cstring>
#include <random>
#include <vector>

using namespace gta6rt::audio_out;

namespace {

constexpr uint32_t kRate = 48000;
const char* kNames[] = {"FL", "FR", "FC", "LFE", "BL", "BR", "FLC", "FRC", "BC", "SL", "SR"};

// Logarithmic sweep 20 Hz .. 20 kHz on one input channel, through matrix + limiter; returns output RMS per
// channel per third of the sweep (low, mid, high), relative to the input RMS of that third.
void sweep_gains(const Matrix& mx, uint32_t in_ch, float amp, double gains[kMaxChannels][3]) {
    const uint32_t n = kRate * 3;
    std::vector<float> in(static_cast<size_t>(n) * mx.in, 0.0f), out(static_cast<size_t>(n) * mx.out, 0.0f);
    const double f0 = 20, f1 = 20000, k = std::log(f1 / f0);
    for (uint32_t i = 0; i < n; ++i) {
        const double t = double(i) / kRate, T = double(n) / kRate;
        const double phase = 2 * M_PI * f0 * T / k * (std::exp(t / T * k) - 1);
        in[size_t(i) * mx.in + in_ch] = amp * static_cast<float>(std::sin(phase));
    }
    float ones[kMaxChannels];
    for (float& g : ones) g = 1.0f;
    Limiter lim;
    lim.configure(0.98f, kRate);
    for (uint32_t at = 0; at < n; at += 512) {
        const uint32_t c = std::min(512u, n - at);
        mix_add(mx, in.data() + size_t(at) * mx.in, ones, out.data() + size_t(at) * mx.out, c);
        lim.process(out.data() + size_t(at) * mx.out, c, mx.out);
    }
    for (int third = 0; third < 3; ++third) {
        const uint32_t a = n / 3 * third, b = n / 3 * (third + 1);
        double ein = 0;
        for (uint32_t i = a; i < b; ++i) ein += double(in[size_t(i) * mx.in + in_ch]) * in[size_t(i) * mx.in + in_ch];
        for (uint32_t o = 0; o < mx.out; ++o) {
            double e = 0;
            for (uint32_t i = a; i < b; ++i) e += double(out[size_t(i) * mx.out + o]) * out[size_t(i) * mx.out + o];
            gains[o][third] = std::sqrt(e / ein);
        }
    }
}

void check_layout(const char* label, const Layout& in, const Layout& out, const float expect[kMaxChannels][kMaxChannels]) {
    const Matrix mx = build_matrix(in, out);
    CHECK(mx.in == in.channels && mx.out == out.channels, "%s: matrix %ux%u", label, mx.out, mx.in);
    for (uint32_t c = 0; c < in.channels; ++c) {
        double g[kMaxChannels][3];
        sweep_gains(mx, c, 0.5f, g);
        for (uint32_t o = 0; o < out.channels; ++o) {
            for (int t = 0; t < 3; ++t) {
                CHECK(std::fabs(g[o][t] - expect[o][c]) < 1e-3, "%s: input %s -> output %u, sweep third %d: gain %.5f, expected %.5f",
                      label, kNames[in.speaker[c]], o, t, g[o][t], expect[o][c]);
            }
        }
        std::printf("%s: sweep on %-3s ->", label, kNames[in.speaker[c]]);
        for (uint32_t o = 0; o < out.channels; ++o) std::printf(" %.4f", g[o][1]);
        std::printf("\n");
    }
}

}  // namespace

int main() {
    const Layout l71 = layout_from_mask(0x63F, 8);
    const float s = 0.70710678f;
    CHECK(l71.speaker[0] == kFL && l71.speaker[3] == kLFE && l71.speaker[4] == kBL && l71.speaker[6] == kSL &&
              l71.speaker[7] == kSR,
          "7.1 mask order");

    {
        const Layout st = layout_from_mask(0x3, 2);
        //                       FL FR FC LFE BL BR SL SR
        const float e[kMaxChannels][kMaxChannels] = {{1, 0, s, 0, s, 0, s, 0}, {0, 1, s, 0, 0, s, 0, s}};
        check_layout("7.1->stereo", l71, st, e);
    }
    {
        const Layout mono = layout_from_mask(0x4, 1);
        const float h = 0.5f, q = 0.5f * s;
        const float e[kMaxChannels][kMaxChannels] = {{h, h, s, 0, q, q, q, q}};
        check_layout("7.1->mono", l71, mono, e);
    }
    {
        const uint32_t labels[] = {1, 2, 3, 4, 5, 6};
        const Layout l51 = layout_from_labels(labels, 6);
        // out: FL FR FC LFE SL SR; back pair folds into the side pair at -3 dB
        const float e[kMaxChannels][kMaxChannels] = {{1, 0, 0, 0, 0, 0, 0, 0}, {0, 1, 0, 0, 0, 0, 0, 0},
                                                     {0, 0, 1, 0, 0, 0, 0, 0}, {0, 0, 0, 1, 0, 0, 0, 0},
                                                     {0, 0, 0, 0, s, 0, 1, 0}, {0, 0, 0, 0, 0, s, 0, 1}};
        check_layout("7.1->5.1(L R C LFE Ls Rs)", l71, l51, e);
    }
    {
        const uint32_t labels[] = {1, 2, 3, 4, 5, 6, 33, 34};
        const Layout dev71 = layout_from_labels(labels, 8);
        float e[kMaxChannels][kMaxChannels] = {};
        // device order L R C LFE Ls Rs Rls Rrs = FL FR FC LFE SL SR BL BR
        const int map[8] = {0, 1, 2, 3, 6, 7, 4, 5};
        for (int i = 0; i < 8; ++i) e[map[i]][i] = 1;
        check_layout("7.1->7.1 device", l71, dev71, e);
        const Matrix id = build_matrix(l71, l71);
        CHECK(id.identity, "7.1 to the same layout is pass-through");
    }
    {
        const Layout st = layout_from_mask(0x3, 2);
        const Matrix mx = build_matrix(st, st);
        CHECK(mx.identity, "stereo to stereo is pass-through");
        std::mt19937 rng(1);
        std::uniform_real_distribution<float> d(-0.98f, 0.98f);
        std::vector<float> in(48000 * 2), out(48000 * 2, 0.0f);
        for (float& x : in) x = d(rng);
        float ones[kMaxChannels] = {1, 1, 1, 1, 1, 1, 1, 1};
        mix_add(mx, in.data(), ones, out.data(), 48000);
        Limiter lim;
        lim.configure(0.98f, kRate);
        const uint32_t limited = lim.process(out.data(), 48000, 2);
        CHECK(limited == 0 && std::memcmp(in.data(), out.data(), in.size() * 4) == 0,
              "stereo pass-through is bit-exact below the limiter threshold (%u frames limited)", limited);
    }
    {
        const Layout st = layout_from_mask(0x3, 2);
        const Matrix mx = build_matrix(l71, st);
        const uint32_t n = 48000;
        std::vector<float> in(size_t(n) * 8), out(size_t(n) * 2, 0.0f);
        for (uint32_t i = 0; i < n; ++i)
            for (int c = 0; c < 8; ++c) in[size_t(i) * 8 + c] = i < n / 2 ? static_cast<float>(std::sin(2 * M_PI * 997 * i / kRate)) : 0.1f * static_cast<float>(std::sin(2 * M_PI * 997 * i / kRate));
        float ones[kMaxChannels] = {1, 1, 1, 1, 1, 1, 1, 1};
        mix_add(mx, in.data(), ones, out.data(), n);
        float pre = 0;
        for (float x : out) pre = std::max(pre, std::fabs(x));
        Limiter lim;
        lim.configure(0.98f, kRate);
        const uint32_t limited = lim.process(out.data(), n, 2);
        float peak = 0;
        for (float x : out) peak = std::max(peak, std::fabs(x));
        CHECK(peak <= 0.98f + 1e-6f, "all 8 channels in phase at full scale: peak %.4f after the limiter (%.3f before)", peak, pre);
        CHECK(limited > 0, "limiter engaged");
        std::printf("worst case 7.1 full scale in phase: %.3f before limiter, %.4f after, %u frames limited, gain at end %.5f\n",
                    pre, peak, limited, lim.gain());
        CHECK(lim.gain() == 1.0f, "limiter released to 1.0 within 0.5 s of quiet input (gain %.5f)", lim.gain());
    }
    {
        Limiter lim;
        lim.configure(0.98f, kRate);
        float x[4] = {NAN, 0.5f, INFINITY, -0.2f};
        lim.process(x, 2, 2);
        CHECK(x[0] == 0 && x[1] == 0, "NaN frame is zeroed");
        CHECK(x[2] == 0 && x[3] == 0, "inf frame is zeroed (%f %f)", x[2], x[3]);
    }
    return finish("audio_out_downmix_test");
}
