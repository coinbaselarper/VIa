// Synthetic XMA2 streams for tests: valid WMA Pro frames (one 512-sample subframe, 60..99 coded coefficients per
// channel; count, quantizer step and signs vary per frame so every frame decodes differently) laid end to end across
// 2048-byte packets the way the encoder does, frames spilling over packet boundaries. Frame layout from
// port/shims_gdk/tests/acphal_test.cpp (write_xma_packet), packetisation from the XMA2 packet header.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace synth {

struct Bits {
    std::vector<uint8_t> buf;
    uint64_t at = 0;
    void put(uint32_t value, uint32_t width) {
        for (uint32_t i = 0; i < width; ++i, ++at) {
            if (buf.size() <= at / 8) buf.push_back(0xFF);
            const auto mask = static_cast<uint8_t>(0x80u >> (at % 8));
            buf[at / 8] &= static_cast<uint8_t>(~mask);
            if ((value >> (width - 1 - i)) & 1u) buf[at / 8] |= mask;
        }
    }
};

inline uint32_t coefficients(uint32_t k) { return 60 + (k * 7) % 40; }

inline uint32_t frame_bits(uint32_t k, bool stereo, uint32_t rate_index) {
    constexpr uint32_t scale_bands[] = {23, 24, 23, 23};
    const uint32_t bands = scale_bands[rate_index];
    const uint32_t channels = stereo ? 2u : 1u;
    return (stereo ? 90 + 2 * bands : 59 + bands) + 3 * (coefficients(k) - 1) * channels;
}

inline void frame(Bits& b, uint32_t k, bool stereo, uint32_t rate_index, bool more) {
    constexpr uint32_t scale_bands[] = {23, 24, 23, 23};
    const uint32_t bands = scale_bands[rate_index];
    const uint32_t channels = stereo ? 2u : 1u;
    b.put(frame_bits(k, stereo, rate_index), 15);
    b.put(0b10u, 2);
    if (stereo) b.put(0, 1);
    b.put(0, 11);
    if (stereo) b.put(0b010u, 3);
    b.put((1u << channels) - 1, channels);
    b.put(1, 1);
    b.put(0, 8 * channels);
    b.put(k % 24, 6);
    if (stereo) b.put(0, 5);
    for (uint32_t c = 0; c < channels; ++c) {
        b.put(0, 2);
        b.put((1u << bands) - 1, bands);
    }
    for (uint32_t c = 0; c < channels; ++c) {
        b.put(0, 1);
        for (uint32_t n = 0; n < coefficients(k); ++n) {
            b.put(0b00u, 2);
            b.put(((k + c + n * n) & 1u), 1);
        }
        b.put(0b0101011u, 7);
    }
    b.put(0, 1);
    b.put(more ? 1 : 0, 1);
}

// `frames` frames packed end to end into full packets (frames spill across packet boundaries). Whole packets.
inline std::vector<uint8_t> stream(uint32_t frames, bool stereo = false, uint32_t rate_index = 1, uint32_t first_k = 0) {
    Bits payload;
    std::vector<uint64_t> starts;
    for (uint32_t k = 0; k < frames; ++k) {
        starts.push_back(payload.at);
        frame(payload, first_k + k, stereo, rate_index, k + 1 < frames);
    }
    const uint64_t total = payload.at;
    const uint64_t per = 2048 * 8 - 32;
    const uint64_t packets = (total + per - 1) / per;
    std::vector<uint8_t> out(packets * 2048, 0xFF);
    std::size_t f = 0;
    for (uint64_t p = 0; p < packets; ++p) {
        const uint64_t lo = p * per, hi = lo + per;
        uint32_t count = 0;
        while (f < starts.size() && starts[f] < hi) {
            ++count;
            ++f;
        }
        uint64_t first = hi;
        for (std::size_t j = 0; j < starts.size(); ++j) {
            if (starts[j] >= lo) {
                first = starts[j];
                break;
            }
        }
        const uint32_t offset = first >= hi ? 0x7FFFu : static_cast<uint32_t>(first - lo);
        const uint32_t header = (count << 26) | (offset << 11) | (1u << 8) | 0u;
        uint8_t* pkt = out.data() + p * 2048;
        pkt[0] = header >> 24, pkt[1] = header >> 16, pkt[2] = header >> 8, pkt[3] = header;
        for (uint64_t bit = lo; bit < hi && bit < total; ++bit) {
            const uint32_t v = (payload.buf[bit / 8] >> (7 - bit % 8)) & 1u;
            const uint64_t dst = 32 + (bit - lo);
            pkt[dst / 8] = static_cast<uint8_t>((pkt[dst / 8] & ~(0x80u >> (dst % 8))) | (v << (7 - dst % 8)));
        }
    }
    return out;
}

}  // namespace synth
