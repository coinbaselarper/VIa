// Drives SHAPE XMA contexts the way game_scarlett_master.exe does (disassembly in port/xma/STATUS.md):
//   submit  exe+0x785d0  context setup per input buffer (rate bits, stereo, 23-block ring, work buffer, packet seek,
//                        frame bit offset, start subframes, loop points from exe+0x43a7440, loop count 255)
//   enable  exe+0x43a73e0 SubmitCommand(5, {index})
//   update  exe+0x78120  available = (write - read) blocks (full when equal and output not valid), copy, then
//                        SubmitCommand(18, {index, blocks})
//   release exe+0x925e0  SubmitCommand(7, {index})
// `Hal` is anything with command(cmd, config) (the engine, or the acphal.dll shim's SubmitCommand); `ApuT` gives
// alloc(bytes) -> APU address and ptr(APU address) (a test arena, or the shim's ApuAlloc/ApuMapApuAddress).
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace title {

constexpr uint32_t kContexts = 350;
constexpr uint32_t kRingBlocks = 23;
constexpr uint32_t kRingBytes = kRingBlocks * 256;
constexpr uint32_t kWorkBytes = 0x200;

inline uint32_t packet_frames(const uint8_t* p) { return p[0] >> 2; }
inline uint32_t first_frame_bit(const uint8_t* p) {
    return 32 + ((uint32_t(p[0] & 3) << 13) | (uint32_t(p[1]) << 5) | (p[2] >> 3));
}
inline uint32_t bits15(const uint8_t* p, uint32_t bit) {
    uint32_t v = 0;
    for (int i = 0; i < 15; ++i, ++bit) v = (v << 1) | ((p[bit >> 3] >> (7 - (bit & 7))) & 1u);
    return v;
}

// exe+0x55cbd70: samples in a buffer (512 per frame of each packet whose metadata is 1).
inline uint32_t buffer_samples(const uint8_t* data, uint32_t bytes) {
    uint32_t total = 0;
    for (uint32_t at = 0; at < bytes; at += 0x800 + 0x800u * data[at + 3]) {
        if ((data[at + 2] & 7) != 1) return 0;
        total += packet_frames(data + at) * 512;
    }
    return total;
}

// The walk shared by exe+0x785d0 and exe+0x43a7440: absolute bit offset of frame `sample >> 9`, or 0.
inline uint32_t frame_bit_offset(const uint8_t* data, uint32_t bytes, uint32_t sample) {
    const uint32_t want = sample >> 9;
    uint32_t before = 0;
    for (uint32_t at = 0; at + 0x800 <= bytes; at += 0x800 + 0x800u * data[at + 3]) {
        const uint32_t n = packet_frames(data + at);
        if (before + n > want) {
            uint32_t bit = first_frame_bit(data + at);
            for (uint32_t k = before; k < want; ++k) bit += bits15(data + at, bit);
            return at * 8 + bit;
        }
        before += n;
    }
    return 0;
}

// exe+0x43a7440 fallback: bit offset of the last frame in the buffer.
inline uint32_t last_frame_bit(const uint8_t* data, uint32_t bytes) {
    uint32_t at = 0, last = 0;
    for (; at + 0x800 <= bytes; at += 0x800 + 0x800u * data[at + 3]) last = at;
    if (packet_frames(data + last) == 0 && last >= 0x800) last -= 0x800;
    uint32_t bit = first_frame_bit(data + last);
    for (uint32_t k = 1; k < packet_frames(data + last); ++k) bit += bits15(data + last, bit);
    return last * 8 + bit;
}

struct Apu {
    std::vector<uint8_t> mem;
    uint32_t base = 0x40000000u;
    uint32_t top = 0;
    explicit Apu(std::size_t bytes) : mem(bytes, 0) {}
    uint32_t alloc(uint32_t bytes, uint32_t align = 0x800) {
        top = (top + align - 1) & ~(align - 1);
        const uint32_t at = top;
        top += bytes;
        return base + at;
    }
    uint8_t* ptr(uint32_t apu) { return mem.data() + (apu - base); }
    static uint8_t* span(void* self, uint32_t apu, std::size_t len) {
        auto* a = static_cast<Apu*>(self);
        if (apu < a->base || apu - a->base + len > a->mem.size()) return nullptr;
        return a->ptr(apu);
    }
};

struct Wave {
    const uint8_t* data = nullptr;  // APU-visible copy of the packets
    uint32_t apu = 0;
    uint32_t bytes = 0;
    uint32_t rate = 32000;
    uint32_t channels = 1;
};

struct Buffer {
    uint32_t start = 0;       // first sample wanted (descriptor +0x14)
    uint32_t end = 0;         // end sample, 0 = whole buffer (+0x18)
    uint32_t loop_start = 0;  // loop start sample (+0x1c)
    uint32_t loop_end = 0;    // loop end sample, 0 = no loop (+0x20)
};

struct Voice {
    uint32_t index = ~0u;
    uint32_t position = 0;  // sample position of the hardware output (+0x1c)
    uint32_t start = 0;     // (+0x14)
    uint32_t end = 0;       // (+0x18)
    uint32_t base = 0;      // samples of the packets the packet seek dropped
    bool enabled = false;
};

template <class Hal, class ApuT = Apu>
struct Title {
    Hal& hal;
    ApuT& apu;
    uint32_t* contexts;
    uint32_t ring[kContexts], work[kContexts];
    bool free_[kContexts];
    uint32_t next_alloc = 0;

    Title(Hal& h, ApuT& a, uint32_t* ctx) : hal(h), apu(a), contexts(ctx) {
        const uint32_t block = apu.alloc(kContexts * (kRingBytes + kWorkBytes));
        for (uint32_t i = 0; i < kContexts; ++i) {
            ring[i] = block + i * (kRingBytes + kWorkBytes);
            work[i] = ring[i] + kRingBytes;
            free_[i] = true;
        }
        std::memset(contexts, 0, kContexts * 64);
    }

    uint32_t* w(uint32_t i) { return contexts + 16 * i; }

    // exe+0x55cbc80: round robin over the free records.
    uint32_t alloc_context() {
        for (uint32_t n = 0; n < kContexts; ++n) {
            const uint32_t i = (next_alloc + n) % kContexts;
            if (free_[i]) {
                free_[i] = false;
                next_alloc = (i + 1) % kContexts;
                return i;
            }
        }
        return ~0u;
    }

    // exe+0x785d0 for one buffer of `wave`.
    Voice submit(const Wave& wave, const Buffer& b) {
        Voice v;
        v.index = alloc_context();
        if (v.index == ~0u) return v;
        uint32_t* c = w(v.index);
        std::memset(c, 0, 64);
        std::memset(apu.ptr(work[v.index]), 0, kWorkBytes);
        if (wave.channels < 3) c[1] = (c[1] & 0xdfffffffu) | (((wave.channels & 1u) << 29) ^ 0x20000000u);
        const uint32_t rate_bits = wave.rate <= 24000 ? 0u : wave.rate <= 32000 ? 1u : wave.rate <= 44100 ? 2u : 3u;
        c[1] = (c[1] & 0xe7ffffffu) | (rate_bits << 27);
        c[1] = (c[1] & 0xff0fffffu) | 0x400000u;
        c[0] = (c[0] & 0xf83fffffu) | (kRingBlocks << 22);
        c[7] = ring[v.index];
        c[1] |= 0x80000000u;
        c[8] = work[v.index];

        const uint8_t* data = wave.data;
        uint32_t apu_data = wave.apu;
        uint32_t bytes = wave.bytes;
        uint32_t skipped = 0;
        uint32_t loop_start_bit = 0, loop_skip = 0, loop_end_bit = 0, loop_sub_end = 0;
        if (b.start != 0 && b.loop_end == 0) {
            // exe+0x55cbd10: drop the packets that end before the start sample.
            uint32_t at = 0, before = 0;
            while (at < bytes) {
                const uint32_t n = packet_frames(data + at) * 512;
                const uint32_t next = at + 0x800 + 0x800u * data[at + 3];
                if (before + n > b.start) break;
                before += n;
                at = next;
            }
            data += at, apu_data += at, bytes -= at, skipped = before;
        } else if (b.loop_end != 0) {
            if (bytes >= 0x800) {
                loop_start_bit = frame_bit_offset(data, bytes, b.loop_start);
                if (loop_start_bit) loop_skip = (b.loop_start >> 7) & 3;
                loop_end_bit = frame_bit_offset(data, bytes, b.loop_end);
                if (loop_end_bit) loop_sub_end = (b.loop_end >> 7) & 3;
            }
            if (!loop_end_bit) loop_end_bit = last_frame_bit(data, bytes);
            c[3] = (c[3] & 0xfc000000u) | loop_start_bit;
            c[4] = (c[4] & 0xfc000000u) | loop_end_bit;
            c[1] = (c[1] & 0xffffcfffu) | (loop_sub_end << 12);
            c[1] = (c[1] & 0xfff1ffffu) | (loop_skip << 17);
            c[0] |= 0xff000u;
        }
        const uint32_t total = buffer_samples(data, bytes);
        uint32_t start = b.start - skipped;
        v.start = start;
        const uint32_t r9 = start > 0x7f ? start - 0x80 : start;
        const uint32_t sub = (r9 >> 7) & 3;
        uint32_t read_bit = 32;
        v.position = sub << 7;
        if (r9 >= 0x200) {
            v.position |= r9 & ~0x1ffu;
            read_bit = bytes >= 0x800 ? frame_bit_offset(data, bytes, r9) : 0;
        }
        v.end = b.end ? b.end - skipped : total;
        v.base = skipped;
        c[4] &= 0x7fffffffu;
        if ((bytes & 0x7ff) == 0 && bytes <= 0x7ff800) c[0] = (c[0] & 0xfffff000u) | (bytes >> 11);
        c[5] = apu_data;
        c[1] &= 0xfffff000u;
        c[6] = apu_data;
        c[0] |= 0x100000u;
        c[0] &= ~(1u << 21);
        c[1] |= 0x80000000u;
        c[2] = (c[2] & 0xfc000000u) | read_bit;
        c[1] = (c[1] & 0xf8ffffffu) | (sub << 24);
        return v;
    }

    void enable(Voice& v) {
        const uint32_t cfg[1] = {v.index};
        hal.command(5, cfg);
        v.enabled = true;
    }

    // exe+0x78120 consumer side: takes the available blocks, appends the s16 samples, advances.
    uint32_t update(Voice& v, std::vector<int16_t>& out) {
        uint32_t* c = w(v.index);
        const uint32_t w0 = __atomic_load_n(&c[0], __ATOMIC_ACQUIRE);
        const uint32_t size = (w0 >> 22) & 31;
        const uint32_t write = (w0 >> 27) & 31;
        const uint32_t read = __atomic_load_n(&c[9], __ATOMIC_ACQUIRE) & 31;
        uint32_t avail;
        if (write > read) avail = write - read;
        else if (write < read) avail = size - read + write;
        else avail = (__atomic_load_n(&c[1], __ATOMIC_ACQUIRE) & 0x80000000u) ? 0 : size;
        if (!avail) return 0;
        const uint8_t* r = apu.ptr(ring[v.index]);
        for (uint32_t k = 0; k < avail; ++k) {
            const auto* blk = reinterpret_cast<const int16_t*>(r + ((read + k) % size) * 256);
            out.insert(out.end(), blk, blk + 128);
        }
        const uint32_t cfg[2] = {v.index, avail};
        hal.command(18, cfg);
        return avail;
    }

    void release(Voice& v) {
        const uint32_t cfg[1] = {v.index};
        hal.command(7, cfg);
        free_[v.index] = true;
        v.enabled = false;
    }

    // Hand the decoder state to the next buffer's context the way exe+0x78120 does (work buffer copy).
    void copy_work(const Voice& from, const Voice& to) {
        std::memcpy(apu.ptr(work[to.index]), apu.ptr(work[from.index]), kWorkBytes);
    }
};

}  // namespace title
