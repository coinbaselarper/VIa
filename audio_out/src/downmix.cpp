#include "downmix.h"

#include <cmath>
#include <cstring>

namespace gta6rt::audio_out {

namespace {
constexpr float kM3dB = 0.70710678f;
}

uint32_t default_mask(uint32_t channels) {
    switch (channels) {
        case 1: return 0x4;
        case 2: return 0x3;
        case 3: return 0x7;
        case 4: return 0x33;
        case 5: return 0x37;
        case 6: return 0x3F;
        case 7: return 0x13F;
        case 8: return 0x63F;
        default: return 0;
    }
}

Layout layout_from_mask(uint32_t mask, uint32_t channels) {
    Layout l;
    l.channels = channels > kMaxChannels ? kMaxChannels : channels;
    if (mask == 0) mask = default_mask(l.channels);
    uint32_t c = 0;
    for (uint8_t bit = 0; bit < 18 && c < l.channels; ++bit) {
        if (mask & (1u << bit)) l.speaker[c++] = bit;
    }
    return l;
}

Layout layout_from_labels(const uint32_t* labels, uint32_t count) {
    Layout l;
    l.channels = count > kMaxChannels ? kMaxChannels : count;
    for (uint32_t i = 0; i < l.channels; ++i) {
        uint8_t s = kNoSpeaker;
        switch (labels[i]) {
            case 1: case 301: s = kFL; break;
            case 2: case 302: s = kFR; break;
            case 3: case 42: s = kFC; break;
            case 4: s = kLFE; break;
            case 5: case 10: s = kSL; break;
            case 6: case 11: s = kSR; break;
            case 7: s = kFLC; break;
            case 8: s = kFRC; break;
            case 9: s = kBC; break;
            case 12: s = kTC; break;
            case 13: s = kTFL; break;
            case 14: s = kTFC; break;
            case 15: s = kTFR; break;
            case 16: s = kTBL; break;
            case 17: s = kTBC; break;
            case 18: s = kTBR; break;
            case 33: s = kBL; break;
            case 34: s = kBR; break;
            default: break;
        }
        bool dup = false;
        for (uint32_t j = 0; j < i; ++j) dup = dup || (s != kNoSpeaker && l.speaker[j] == s);
        l.speaker[i] = dup ? kNoSpeaker : s;
    }
    return l;
}

namespace {

struct Fold {
    uint8_t to[2];
    float gain[2];
    int n;
};

void add(Fold& f, uint8_t s, float g) {
    f.to[f.n] = s;
    f.gain[f.n] = g;
    ++f.n;
}

bool has(const Layout& o, uint8_t s) { return o.find(s) >= 0; }

// Where speaker s goes in an output without s.
Fold fold(uint8_t s, const Layout& o) {
    Fold f{};
    const bool left = (s == kSL || s == kBL || s == kFLC || s == kTFL || s == kTBL);
    switch (s) {
        case kFC: case kTC: case kTFC:
            if (s != kFC && has(o, kFC)) { add(f, kFC, kM3dB); break; }
            add(f, kFL, kM3dB);
            add(f, kFR, kM3dB);
            break;
        case kLFE:
            break;
        case kSL: case kSR:
            if (has(o, left ? kBL : kBR)) add(f, left ? kBL : kBR, 1.0f);
            else add(f, left ? kFL : kFR, kM3dB);
            break;
        case kBL: case kBR:
            if (has(o, left ? kSL : kSR)) add(f, left ? kSL : kSR, kM3dB);
            else add(f, left ? kFL : kFR, kM3dB);
            break;
        case kBC: case kTBC:
            if (has(o, kBL) && has(o, kBR)) { add(f, kBL, kM3dB); add(f, kBR, kM3dB); }
            else if (has(o, kSL) && has(o, kSR)) { add(f, kSL, kM3dB); add(f, kSR, kM3dB); }
            else { add(f, kFL, 0.5f); add(f, kFR, 0.5f); }
            break;
        case kFLC: case kFRC:
            add(f, left ? kFL : kFR, 1.0f);
            break;
        case kTFL: case kTFR: case kTBL: case kTBR:
            add(f, left ? kFL : kFR, kM3dB);
            break;
        default:
            break;
    }
    return f;
}

}  // namespace

Matrix build_matrix(const Layout& in, const Layout& out) {
    Matrix mx;
    mx.in = in.channels;
    mx.out = out.channels;
    const bool mono = out.channels == 1;
    Layout stereo;
    stereo.channels = 2;
    stereo.speaker[0] = kFL;
    stereo.speaker[1] = kFR;
    const Layout& target = mono ? stereo : out;
    float tmp[kMaxChannels][kMaxChannels] = {};
    for (uint32_t i = 0; i < in.channels; ++i) {
        const uint8_t s = in.speaker[i];
        if (s == kNoSpeaker) continue;
        const int direct = target.find(s);
        if (direct >= 0) {
            tmp[direct][i] = 1.0f;
            continue;
        }
        const Fold f = fold(s, target);
        for (int k = 0; k < f.n; ++k) {
            const int o = target.find(f.to[k]);
            if (o >= 0) tmp[o][i] += f.gain[k];
        }
    }
    if (mono) {
        for (uint32_t i = 0; i < in.channels; ++i) mx.m[0][i] = 0.5f * (tmp[0][i] + tmp[1][i]);
        if (in.channels == 1 && in.speaker[0] == kFC) mx.m[0][0] = 1.0f;
    } else {
        std::memcpy(mx.m, tmp, sizeof tmp);
    }
    bool id = mx.in == mx.out;
    for (uint32_t o = 0; o < mx.out && id; ++o)
        for (uint32_t i = 0; i < mx.in && id; ++i) id = mx.m[o][i] == (o == i ? 1.0f : 0.0f);
    mx.identity = id;
    return mx;
}

void mix_add(const Matrix& mx, const float* in, const float* gains, float* out, uint32_t frames) {
    const uint32_t ic = mx.in, oc = mx.out;
    if (mx.identity) {
        for (uint32_t f = 0; f < frames; ++f)
            for (uint32_t c = 0; c < ic; ++c) out[f * oc + c] += in[f * ic + c] * gains[c];
        return;
    }
    float g[kMaxChannels][kMaxChannels];
    for (uint32_t o = 0; o < oc; ++o)
        for (uint32_t i = 0; i < ic; ++i) g[o][i] = mx.m[o][i] * gains[i];
    for (uint32_t f = 0; f < frames; ++f) {
        const float* src = in + f * ic;
        float* dst = out + f * oc;
        for (uint32_t o = 0; o < oc; ++o) {
            float acc = 0.0f;
            for (uint32_t i = 0; i < ic; ++i) acc += g[o][i] * src[i];
            dst[o] += acc;
        }
    }
}

void Limiter::configure(float threshold, uint32_t rate) {
    threshold_ = threshold;
    release_ = 1.0f - std::exp(-1.0f / (0.05f * static_cast<float>(rate ? rate : 48000)));
    env_ = 1.0f;
}

uint32_t Limiter::process(float* frames, uint32_t count, uint32_t channels) {
    uint32_t limited = 0;
    for (uint32_t f = 0; f < count; ++f) {
        float* x = frames + f * channels;
        float peak = 0.0f;
        bool finite = true;
        for (uint32_t c = 0; c < channels; ++c) {
            const float a = std::fabs(x[c]);
            finite = finite && std::isfinite(a);
            peak = a > peak ? a : peak;
        }
        if (!finite) {
            for (uint32_t c = 0; c < channels; ++c) x[c] = 0.0f;
            continue;
        }
        if (peak * env_ > threshold_) env_ = threshold_ / peak;
        if (env_ < 1.0f) {
            ++limited;
            for (uint32_t c = 0; c < channels; ++c) x[c] *= env_;
            env_ += (1.0f - env_) * release_;
            if (env_ > 0.9999f) env_ = 1.0f;
        }
    }
    return limited;
}

}  // namespace gta6rt::audio_out
