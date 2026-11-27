#pragma once

#include <cstdint>

namespace gta6rt::audio_out {

constexpr uint32_t kMaxChannels = 8;
constexpr uint8_t kNoSpeaker = 0xff;

// WAVE speaker positions (bit index in a WAVEFORMATEXTENSIBLE channel mask).
enum Speaker : uint8_t {
    kFL = 0, kFR = 1, kFC = 2, kLFE = 3, kBL = 4, kBR = 5, kFLC = 6, kFRC = 7, kBC = 8, kSL = 9, kSR = 10,
    kTC = 11, kTFL = 12, kTFC = 13, kTFR = 14, kTBL = 15, kTBC = 16, kTBR = 17,
};

struct Layout {
    uint32_t channels = 0;
    uint8_t speaker[kMaxChannels] = {kNoSpeaker, kNoSpeaker, kNoSpeaker, kNoSpeaker,
                                     kNoSpeaker, kNoSpeaker, kNoSpeaker, kNoSpeaker};
    int find(uint8_t s) const {
        for (uint32_t i = 0; i < channels; ++i)
            if (speaker[i] == s) return static_cast<int>(i);
        return -1;
    }
};

uint32_t default_mask(uint32_t channels);
Layout layout_from_mask(uint32_t mask, uint32_t channels);
// CoreAudio channel labels (AudioChannelLabel values) to WAVE positions; unknown labels stay kNoSpeaker.
Layout layout_from_labels(const uint32_t* labels, uint32_t count);

struct Matrix {
    uint32_t in = 0, out = 0;
    float m[kMaxChannels][kMaxChannels] = {};  // m[out][in]
    bool identity = false;
};

// ITU-R BS.775 fold-down: a speaker the output has passes at 1.0; FC folds to FL/FR at -3 dB, side and back
// surrounds fold to the nearest present surround or to the front pair at -3 dB, LFE is dropped unless the output
// has one. A mono output takes 0.5 * (L + R) of the stereo fold-down.
Matrix build_matrix(const Layout& in, const Layout& out);

// out (out.out channels, interleaved) += matrix * in (in channels, interleaved) * gains[in channel].
void mix_add(const Matrix& mx, const float* in, const float* gains, float* out, uint32_t frames);

// Peak limiter: instant attack, ~50 ms release, output never above `threshold`. Below threshold the samples pass
// unchanged (gain exactly 1).
class Limiter {
public:
    void configure(float threshold, uint32_t rate);
    // Returns the number of frames whose gain was below 1.
    uint32_t process(float* frames, uint32_t count, uint32_t channels);
    float gain() const { return env_; }

private:
    float threshold_ = 0.98f;
    float release_ = 0.0f;
    float env_ = 1.0f;
};

}  // namespace gta6rt::audio_out
