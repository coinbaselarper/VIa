#include "gta6rt/xma.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stop_token>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kPacketBytes = 2048;
constexpr uint32_t kPacketBits = kPacketBytes * 8;
constexpr uint32_t kHeaderBits = 32;
constexpr uint32_t kMaxFrameBits = kPacketBits - kHeaderBits;
constexpr uint32_t kBlockBytes = 256;
constexpr uint32_t kBlockSamples = kBlockBytes / 2;
constexpr uint32_t kFrameSamples = 512;
constexpr uint32_t kSubframeSamples = 128;
constexpr uint32_t kRates[4] = {24000, 32000, 44100, 48000};
constexpr uint32_t kErrorInputInvalid = 4;
constexpr uint32_t kWorkMagic = 0x414D5847u;
constexpr uint32_t kWorkTagBytes = 16;

constexpr uint32_t kCmdEnableXma = 5;
constexpr uint32_t kCmdEnableXmas = 6;
constexpr uint32_t kCmdDisableXma = 7;
constexpr uint32_t kCmdDisableXmas = 8;
constexpr uint32_t kCmdAdvanceXmaRead = 18;

uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

int env_int(const char* name, int fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    return std::atoi(v);
}

bool log_enabled() {
    static const bool on = env_int("GTA6RT_XMA_LOG", 0) != 0;
    return on;
}

void logf(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[rt] xma: %s\n", line);
}

struct Ctx {
    uint32_t* w = nullptr;
    uint32_t get(std::size_t i) const { return std::atomic_ref(w[i]).load(); }
    uint32_t field(std::size_t i, uint32_t shift, uint32_t bits) const {
        return (get(i) >> shift) & ((bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u));
    }
    uint32_t set_field(std::size_t i, uint32_t shift, uint32_t bits, uint32_t value) const {
        const uint32_t mask = ((bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u)) << shift;
        std::atomic_ref word(w[i]);
        uint32_t prev = word.load();
        while (!word.compare_exchange_weak(prev, (prev & ~mask) | ((value << shift) & mask))) {}
        return prev;
    }
};

uint32_t packet_frames(const uint8_t* p) { return p[0] >> 2; }
uint32_t packet_first_bit(const uint8_t* p) {
    return kHeaderBits + ((static_cast<uint32_t>(p[0] & 3u) << 13) | (static_cast<uint32_t>(p[1]) << 5) | (p[2] >> 3));
}
uint32_t packet_skip(const uint8_t* p) { return p[3]; }

uint32_t read_bits(const uint8_t* p, uint32_t bit, uint32_t n) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; ++i, ++bit) v = (v << 1) | ((p[bit >> 3] >> (7 - (bit & 7))) & 1u);
    return v;
}

void copy_bits(uint8_t* dst, uint32_t dbit, const uint8_t* src, uint32_t sbit, uint32_t n) {
    while (n && (dbit & 7)) {
        const uint32_t b = (src[sbit >> 3] >> (7 - (sbit & 7))) & 1u;
        dst[dbit >> 3] = static_cast<uint8_t>((dst[dbit >> 3] & ~(0x80u >> (dbit & 7))) | (b << (7 - (dbit & 7))));
        ++dbit, ++sbit, --n;
    }
    const uint32_t shift = sbit & 7;
    while (n >= 8) {
        const uint32_t hi = src[sbit >> 3];
        const uint32_t lo = shift ? src[(sbit >> 3) + 1] : 0;
        dst[dbit >> 3] = static_cast<uint8_t>(((hi << shift) | (lo >> (8 - shift))) & 0xFFu);
        dbit += 8, sbit += 8, n -= 8;
    }
    while (n) {
        const uint32_t b = (src[sbit >> 3] >> (7 - (sbit & 7))) & 1u;
        dst[dbit >> 3] = static_cast<uint8_t>((dst[dbit >> 3] & ~(0x80u >> (dbit & 7))) | (b << (7 - (dbit & 7))));
        ++dbit, ++sbit, --n;
    }
}

struct DecoderFree {
    void operator()(Gta6rtXmaDecoder* d) const noexcept { gta6rt_xma_decoder_destroy(d); }
};

struct Voice {
    uint32_t format = 0;
    uint32_t generation = 0;
    std::unique_ptr<Gta6rtXmaDecoder, DecoderFree> decoder;
    bool decoder_failed = false;
    std::vector<int16_t> pcm;
    std::size_t pcm_read = 0;
    bool ended = false;
    bool starved = false;
    bool initial_pending = true;
    uint32_t loop_skip_pending = 0;
    uint32_t errors_logged = 0;
    uint64_t frames = 0;
    uint64_t samples = 0;
    uint64_t loops = 0;
    uint64_t enabled_ns = 0;
    std::vector<float> state;

    uint32_t channels() const { return 1u + ((format >> 2) & 1u); }
    uint32_t rate() const { return kRates[format & 3u]; }
    std::size_t queued() const { return pcm.size() - pcm_read; }
};

struct Slot {
    std::mutex mutex;
    std::mutex ring;
    std::optional<Voice> voice;
    std::atomic<bool> active{false};
};

struct Snapshot {
    std::mutex mutex;
    uint32_t generation = 0;
    uint32_t format = 0;
    std::vector<float> state;
};

struct Stats {
    std::atomic<uint64_t> enables{0}, disables{0}, advances{0}, frames{0}, samples_out{0}, loops{0},
        decode_errors{0}, state_imports{0}, starved{0}, decode_ns{0};
    std::atomic<uint32_t> active{0}, peak_active{0}, workers{0};
};

class Engine {
public:
    ~Engine() { unbind(); }

    int bind(uint32_t* words, uint32_t count, gta6rt_xma_span_fn span, void* user) {
        unbind();
        std::vector<std::unique_ptr<Slot>> slots(count);
        std::vector<std::unique_ptr<Snapshot>> snaps(count);
        std::vector<uint32_t> gens(count, 0);
        for (auto& s : slots) s = std::make_unique<Slot>();
        for (auto& s : snaps) s = std::make_unique<Snapshot>();
        std::unique_lock lock(table_);
        words_ = words;
        count_ = count;
        span_ = span;
        user_ = user;
        slots_.swap(slots);
        snaps_.swap(snaps);
        gens_.swap(gens);
        bind_ns_ = now_ns();
        logf("decoding SHAPE XMA on the host: %u contexts, %d workers on demand, activity log %s", count,
             worker_count(), log_enabled() ? "on" : "off (GTA6RT_XMA_LOG=1)");
        return 0;
    }

    void unbind() {
        stop_workers();
        std::unique_lock lock(table_);
        if (words_ && log_enabled()) summary("unbind");
        words_ = nullptr;
        count_ = 0;
        slots_.clear();
        snaps_.clear();
        gens_.clear();
        stats_.active.store(0);
    }

    int command(uint32_t cmd, const void* config) {
        auto word = [config](std::size_t i) {
            uint32_t v = 0;
            if (config) std::memcpy(&v, static_cast<const uint8_t*>(config) + i * 4, 4);
            return v;
        };
        switch (cmd) {
            case kCmdEnableXma:
                if (config) enable(word(0));
                return 1;
            case kCmdDisableXma:
                if (config) disable(word(0));
                return 1;
            case kCmdEnableXmas:
            case kCmdDisableXmas: {
                uint32_t count;
                {
                    std::shared_lock lock(table_);
                    count = count_;
                }
                if (!config) return 1;
                for (uint32_t i = 0; i < count; ++i) {
                    if (word(i / 32) & (1u << (i % 32))) cmd == kCmdEnableXmas ? enable(i) : disable(i);
                }
                return 1;
            }
            case kCmdAdvanceXmaRead:
                if (config) advance(word(0), word(1));
                return 1;
            default:
                return 0;
        }
    }

    void stats(gta6rt_xma_stats* out) const {
        out->enables = stats_.enables.load();
        out->disables = stats_.disables.load();
        out->advances = stats_.advances.load();
        out->frames = stats_.frames.load();
        out->samples_out = stats_.samples_out.load();
        out->loops = stats_.loops.load();
        out->decode_errors = stats_.decode_errors.load();
        out->state_imports = stats_.state_imports.load();
        out->starved = stats_.starved.load();
        out->decode_ns = stats_.decode_ns.load();
        out->active = stats_.active.load();
        out->peak_active = stats_.peak_active.load();
        out->workers = stats_.workers.load();
    }

private:
    static int worker_count() { return std::clamp(env_int("GTA6RT_XMA_THREADS", 4), 1, 8); }

    Ctx ctx_locked(uint32_t i) const { return (words_ && i < count_) ? Ctx{words_ + 16u * i} : Ctx{}; }

    uint8_t* span(uint32_t apu, std::size_t len) const { return span_ ? span_(user_, apu, len) : nullptr; }

    void enable(uint32_t i) {
        std::shared_lock table(table_);
        Ctx ctx = ctx_locked(i);
        if (!ctx.w) {
            logf("ENABLE_XMA for context %u outside the %u bound contexts: ignored", i, count_);
            return;
        }
        bool first = false;
        {
            std::lock_guard lock(slots_[i]->mutex);
            auto& v = slots_[i]->voice;
            const uint32_t format = ctx.field(1, 27, 3);
            if (!v) {
                v.emplace();
                v->format = format;
                v->generation = ++gens_[i];
                v->enabled_ns = now_ns();
                const uint32_t n = stats_.active.fetch_add(1) + 1;
                uint32_t peak = stats_.peak_active.load();
                while (n > peak && !stats_.peak_active.compare_exchange_weak(peak, n)) {}
                slots_[i]->active.store(true);
            } else if (v->format != format) {
                v->format = format;
                v->decoder.reset();
                v->pcm.clear();
                v->pcm_read = 0;
                v->loop_skip_pending = 0;
            }
            v->ended = false;
            v->starved = false;
            v->initial_pending = true;
            ctx.set_field(2, 26, 6, 0);
            first = stats_.enables.fetch_add(1) == 0;
            if (log_enabled() && (stats_.enables.load() <= 64 || stats_.enables.load() % 1000 == 0)) {
                logf("%sENABLE ctx %u gen %u: %u Hz %u ch, in0 %u pkts @%08x valid %u, read bit %u, start skip %u "
                     "sub, loops %u [%u..%u] sub-end %u loop-skip %u, ring %u blocks @%08x, t=%.3f s",
                     first ? "first " : "", i, v->generation, v->rate(), v->channels(), ctx.field(0, 0, 12),
                     ctx.get(5), ctx.field(0, 20, 2), ctx.field(2, 0, 26), ctx.field(1, 24, 3), ctx.field(0, 12, 8),
                     ctx.field(3, 0, 26), ctx.field(4, 0, 26), ctx.field(1, 12, 2), ctx.field(1, 17, 3),
                     ctx.field(0, 22, 5), ctx.get(7), (now_ns() - bind_ns_) / 1e9);
            }
        }
        start_workers_and_wake();
    }

    void disable(uint32_t i) {
        std::shared_lock table(table_);
        if (i >= count_) {
            logf("DISABLE_XMA for context %u outside the %u bound contexts: ignored", i, count_);
            return;
        }
        std::lock_guard lock(slots_[i]->mutex);
        auto& v = slots_[i]->voice;
        if (!v) return;
        stats_.disables.fetch_add(1);
        if (log_enabled() && (stats_.disables.load() <= 64 || stats_.disables.load() % 1000 == 0)) {
            logf("DISABLE ctx %u gen %u after %.3f s: %llu frames, %llu samples, %llu loops%s", i, v->generation,
                 (now_ns() - v->enabled_ns) / 1e9, (unsigned long long)v->frames, (unsigned long long)v->samples,
                 (unsigned long long)v->loops, v->ended ? ", input ran dry" : "");
        }
        v.reset();
        slots_[i]->active.store(false);
        stats_.active.fetch_sub(1);
    }

    void advance(uint32_t i, uint32_t blocks) {
        std::shared_lock table(table_);
        Ctx ctx = ctx_locked(i);
        if (!ctx.w) return;
        stats_.advances.fetch_add(1);
        {
            std::lock_guard lock(slots_[i]->ring);
            const uint32_t size = ctx.field(0, 22, 5);
            if (size == 0) return;
            ctx.set_field(9, 0, 5, (ctx.field(9, 0, 5) + blocks % size) % size);
            if (blocks > 0) ctx.set_field(1, 31, 1, 1);
        }
        if (slots_[i]->active.load()) wake_workers();
    }

    struct Input {
        const uint8_t* data = nullptr;
        uint32_t packets = 0;
    };

    Input input(Ctx ctx, uint32_t b) const {
        Input in;
        in.packets = b == 0 ? ctx.field(0, 0, 12) : ctx.field(1, 0, 12);
        if (in.packets) in.data = span(ctx.get(5 + b), std::size_t(in.packets) * kPacketBytes);
        return in;
    }

    // The packet `index` of the stream that runs on from buffer `cur` into the other buffer when valid.
    const uint8_t* stream_packet(Ctx ctx, uint32_t cur, uint32_t index) const {
        const Input in = input(ctx, cur);
        if (index < in.packets) return in.data ? in.data + std::size_t(index) * kPacketBytes : nullptr;
        const uint32_t other = cur ^ 1u;
        if (!ctx.field(0, 20 + other, 1)) return nullptr;
        const Input next = input(ctx, other);
        index -= in.packets;
        return (next.data && index < next.packets) ? next.data + std::size_t(index) * kPacketBytes : nullptr;
    }

    static void retire(Ctx ctx, uint32_t cur, uint32_t next_offset) {
        ctx.set_field(2, 0, 26, next_offset);
        const uint32_t other = cur ^ 1u;
        const uint32_t before = ctx.set_field(0, 20 + cur, 1, 0);
        ctx.set_field(4, 31, 1, (before & (1u << (20 + other))) ? other : 0u);
    }

    void voice_error(uint32_t i, Voice& v, const char* fmt, ...) {
        stats_.decode_errors.fetch_add(1);
        if (v.errors_logged++ >= 4) return;
        char msg[384];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        logf("ctx %u gen %u: %s", i, v.generation, msg);
    }

    bool ensure_decoder(uint32_t i, Ctx ctx, Voice& v) {
        if (v.decoder) return true;
        if (v.decoder_failed) return false;
        Gta6rtXmaDecoder* raw = nullptr;
        const int32_t st = gta6rt_xma_decoder_create(v.rate(), v.channels(), &raw);
        if (st != GTA6RT_XMA_OK) {
            v.decoder_failed = true;
            voice_error(i, v, "decoder create failed (status %d, %u Hz, %u ch): silent voice", st, v.rate(),
                        v.channels());
            return false;
        }
        v.decoder.reset(raw);
        v.state.assign(gta6rt_xma_decoder_state_len(raw), 0.0f);
        if (uint8_t* work = span(ctx.get(8), kWorkTagBytes)) {
            uint32_t tag[4];
            std::memcpy(tag, work, sizeof(tag));
            if (tag[0] == kWorkMagic && tag[1] < count_ && tag[1] != i) {
                Snapshot& s = *snaps_[tag[1]];
                std::lock_guard lock(s.mutex);
                if (s.generation == tag[2] && s.format == v.format && s.state.size() == v.state.size() &&
                    gta6rt_xma_decoder_import(raw, s.state.data(), s.state.size()) == GTA6RT_XMA_OK) {
                    stats_.state_imports.fetch_add(1);
                } else if (log_enabled()) {
                    logf("ctx %u: work buffer names ctx %u gen %u, not importable (now gen %u, format %u vs %u)", i,
                         tag[1], tag[2], s.generation, s.format, v.format);
                }
            }
            const uint32_t mine[4] = {kWorkMagic, i, v.generation, v.format};
            std::memcpy(work, mine, sizeof(mine));
        }
        return true;
    }

    void publish_state(uint32_t i, Voice& v) {
        if (!v.decoder || v.state.empty()) return;
        if (gta6rt_xma_decoder_export(v.decoder.get(), v.state.data(), v.state.size()) != GTA6RT_XMA_OK) return;
        Snapshot& s = *snaps_[i];
        std::lock_guard lock(s.mutex);
        s.generation = v.generation;
        s.format = v.format;
        s.state = v.state;
    }

    // Decodes the frame at the context's read offset and queues the subframes the context selects. Returns false
    // when nothing can progress until the title changes the context (input ran dry, or a frame waits for data).
    bool step(uint32_t i, Ctx ctx, Voice& v) {
        for (int guard = 0; guard < 8; ++guard) {
            const uint32_t valid = ctx.field(0, 20, 2);
            if (v.ended) {
                if (valid == 0) return false;
                v.ended = false;
                if (ctx.field(2, 26, 5) == kErrorInputInvalid) ctx.set_field(2, 26, 6, 0);
            }
            if (valid == 0) {
                v.ended = true;
                ctx.set_field(2, 26, 6, kErrorInputInvalid | 32u);
                return false;
            }
            uint32_t cur = ctx.field(4, 31, 1);
            if (!(valid & (1u << cur))) {
                cur ^= 1u;
                ctx.set_field(4, 31, 1, cur);
            }
            const Input in = input(ctx, cur);
            const uint32_t offset = ctx.field(2, 0, 26);
            const uint32_t p = offset / kPacketBits;
            if (!in.data || p >= in.packets) {
                if (!in.data) voice_error(i, v, "input %u (%u packets at %08x) is not APU memory: retired", cur,
                                          in.packets, ctx.get(5 + cur));
                retire(ctx, cur, (p >= in.packets ? p - in.packets : 0) * kPacketBits);
                continue;
            }
            const uint8_t* pkt = in.data + std::size_t(p) * kPacketBytes;
            const uint32_t fc = packet_frames(pkt);
            const uint32_t first = packet_first_bit(pkt);
            const uint32_t next_packet = p + 1 + (packet_skip(pkt) == 0xFFu ? 0u : packet_skip(pkt));
            auto move_to_packet = [&](uint32_t q) {
                if (q >= in.packets) {
                    retire(ctx, cur, (q - in.packets) * kPacketBits);
                } else {
                    ctx.set_field(2, 0, 26, q * kPacketBits + packet_first_bit(in.data + std::size_t(q) * kPacketBytes));
                }
            };
            if (packet_skip(pkt) == 0xFFu || fc == 0 || first >= kPacketBits) {
                move_to_packet(p + 1);
                continue;
            }
            uint32_t rel = std::max(offset % kPacketBits, first);
            uint32_t pos = first, index = 0;
            while (index < fc && pos < rel) {
                const uint32_t len = read_bits(pkt, pos, 15);
                if (len == 0 || len == 0x7FFFu) break;
                pos += len;
                ++index;
            }
            if (index >= fc || pos != rel) {
                voice_error(i, v, "read offset %u is not a frame start in packet %u (%u frames from bit %u): "
                            "moved to the next packet", offset, p, fc, first);
                move_to_packet(next_packet);
                continue;
            }

            const uint8_t* segs[3] = {pkt, nullptr, nullptr};
            uint32_t seg_from[3] = {rel, kHeaderBits, kHeaderBits};
            uint32_t avail = kPacketBits - rel;
            uint32_t nseg = 1;
            uint32_t q = next_packet;
            auto extend = [&]() {
                if (nseg == 3) return false;
                const uint8_t* np = stream_packet(ctx, cur, q);
                if (!np) return false;
                segs[nseg++] = np;
                avail += kPacketBits - kHeaderBits;
                q = q + 1 + (packet_skip(np) == 0xFFu ? 0u : packet_skip(np));
                return true;
            };
            uint8_t synth[kPacketBytes];
            std::memset(synth, 0xFF, sizeof(synth));
            auto gather = [&](uint32_t n) {
                uint32_t dst = kHeaderBits, s = 0, from = seg_from[0];
                while (n) {
                    const uint32_t take = std::min(n, kPacketBits - from);
                    copy_bits(synth, dst, segs[s], from, take);
                    dst += take, n -= take;
                    if (n) from = seg_from[++s];
                }
            };
            if (avail < 15 && !extend()) {
                v.starved = true;
                stats_.starved.fetch_add(1);
                return false;
            }
            gather(15);
            const uint32_t len = read_bits(synth, kHeaderBits, 15);
            if (len < 16 || len > kMaxFrameBits) {
                voice_error(i, v, "frame length %u at bit %u of packet %u: moved to the next packet", len, rel, p);
                move_to_packet(next_packet);
                continue;
            }
            while (avail < len) {
                if (!extend()) {
                    v.starved = true;
                    stats_.starved.fetch_add(1);
                    return false;
                }
            }
            std::memset(synth, 0xFF, sizeof(synth));
            synth[0] = 1u << 2;
            synth[1] = 0;
            synth[2] = 1;
            synth[3] = 0;
            gather(len);

            const uint32_t loops = ctx.field(0, 12, 8);
            const bool loop_end = loops > 0 && offset == std::max(kHeaderBits, ctx.field(4, 0, 26));
            const uint32_t ch = v.channels();
            int16_t frame[kFrameSamples * 2];
            uint32_t frames = 0;
            int32_t st = GTA6RT_XMA_DECODE_ERROR;
            if (ensure_decoder(i, ctx, v)) {
                const uint64_t t0 = now_ns();
                st = gta6rt_xma_decoder_packet(v.decoder.get(), synth, frame, kFrameSamples * ch, &frames);
                stats_.decode_ns.fetch_add(now_ns() - t0);
            }
            if (st != GTA6RT_XMA_OK || frames != 1) {
                if (v.decoder) {
                    voice_error(i, v, "frame at bit %u (%u bits) of packet %u decoded to %u frames (status %d): "
                                "512 samples of silence, decoder reset", rel, len, p, frames, st);
                    gta6rt_xma_decoder_reset(v.decoder.get());
                }
                std::memset(frame, 0, sizeof(frame));
            } else {
                publish_state(i, v);
            }
            ++v.frames;
            stats_.frames.fetch_add(1);

            uint32_t begin = 0, end = 4;
            if (v.initial_pending) {
                const uint32_t skip = ctx.field(1, 24, 3);
                const uint32_t take = std::min(skip, 4u);
                begin = take;
                ctx.set_field(1, 24, 3, skip - take);
                v.initial_pending = skip - take > 0;
            }
            if (v.loop_skip_pending) {
                begin = std::max(begin, std::min(v.loop_skip_pending, 4u));
                v.loop_skip_pending = 0;
            }
            if (loop_end) end = std::min(4u, ctx.field(1, 12, 2) + 1u);
            if (begin < end) {
                v.pcm.insert(v.pcm.end(), frame + begin * kSubframeSamples * ch, frame + end * kSubframeSamples * ch);
            }

            if (loop_end) {
                ctx.set_field(2, 0, 26, std::max(kHeaderBits, ctx.field(3, 0, 26)));
                v.loop_skip_pending = ctx.field(1, 17, 3);
                if (loops != 255u) ctx.set_field(0, 12, 8, loops - 1u);
                ++v.loops;
                stats_.loops.fetch_add(1);
            } else if (index + 1 < fc) {
                ctx.set_field(2, 0, 26, p * kPacketBits + rel + len);
            } else {
                move_to_packet(next_packet);
            }
            return true;
        }
        return false;
    }

    void pump(uint32_t i, Ctx ctx, Voice& v, std::mutex& ring_lock) {
        const uint32_t size = ctx.field(0, 22, 5);
        if (size == 0 || ctx.field(1, 31, 1) == 0) return;
        uint8_t* ring = span(ctx.get(7), std::size_t(size) * kBlockBytes);
        if (!ring) {
            voice_error(i, v, "output ring %08x (%u blocks) is not APU memory: no output", ctx.get(7), size);
            return;
        }
        for (uint32_t n = 0; n < size && ctx.field(1, 31, 1) == 1; ++n) {
            int guard = 0;
            while (v.queued() < kBlockSamples && guard++ < 64) {
                if (!step(i, ctx, v)) break;
            }
            if (v.queued() == 0) return;
            if (v.queued() < kBlockSamples && !v.ended) return;
            std::lock_guard rl(ring_lock);
            if (ctx.field(1, 31, 1) == 0) return;
            const uint32_t write = ctx.field(0, 27, 5) % size;
            uint8_t* out = ring + std::size_t(write) * kBlockBytes;
            const std::size_t take = std::min<std::size_t>(kBlockSamples, v.queued());
            std::memcpy(out, v.pcm.data() + v.pcm_read, take * 2);
            if (take < kBlockSamples) std::memset(out + take * 2, 0, (kBlockSamples - take) * 2);
            v.pcm_read += take;
            v.samples += take;
            stats_.samples_out.fetch_add(take);
            if (v.pcm_read > 4096) {
                v.pcm.erase(v.pcm.begin(), v.pcm.begin() + static_cast<std::ptrdiff_t>(v.pcm_read));
                v.pcm_read = 0;
            }
            const uint32_t next = (write + 1) % size;
            ctx.set_field(0, 27, 5, next);
            if (next == ctx.field(9, 0, 5) % size) ctx.set_field(1, 31, 1, 0);
        }
    }

    void pump_share(uint32_t k, uint32_t n) {
        std::shared_lock table(table_);
        if (!words_) return;
        for (uint32_t i = k; i < count_; i += n) {
            if (!slots_[i]->active.load(std::memory_order_relaxed)) continue;
            std::lock_guard lock(slots_[i]->mutex);
            auto& v = slots_[i]->voice;
            if (!v) continue;
            try {
                pump(i, ctx_locked(i), *v, slots_[i]->ring);
            } catch (const std::bad_alloc&) {
                voice_error(i, *v, "out of memory: voice retired");
                v.reset();
                slots_[i]->active.store(false);
                stats_.active.fetch_sub(1);
                ctx_locked(i).set_field(2, 26, 6, kErrorInputInvalid | 32u);
            }
        }
    }

    void summary(const char* why) const {
        logf("%s: enables %llu disables %llu advances %llu active %u (peak %u) frames %llu samples %llu loops %llu "
             "imports %llu starved %llu errors %llu decode %.1f ms",
             why, (unsigned long long)stats_.enables.load(), (unsigned long long)stats_.disables.load(),
             (unsigned long long)stats_.advances.load(), stats_.active.load(), stats_.peak_active.load(),
             (unsigned long long)stats_.frames.load(), (unsigned long long)stats_.samples_out.load(),
             (unsigned long long)stats_.loops.load(), (unsigned long long)stats_.state_imports.load(),
             (unsigned long long)stats_.starved.load(), (unsigned long long)stats_.decode_errors.load(),
             stats_.decode_ns.load() / 1e6);
    }

    void start_workers_and_wake() {
        {
            std::lock_guard lock(worker_mutex_);
            if (workers_.empty()) {
                const int n = worker_count();
                for (int k = 0; k < n; ++k) {
                    workers_.emplace_back([this, k, n](std::stop_token stop) { worker(stop, k, n); });
                }
                stats_.workers.store(static_cast<uint32_t>(n));
            }
            ++wake_seq_;
        }
        cv_.notify_all();
    }

    void wake_workers() {
        {
            std::lock_guard lock(worker_mutex_);
            ++wake_seq_;
        }
        cv_.notify_all();
    }

    void stop_workers() {
        std::vector<std::jthread> join;
        {
            std::lock_guard lock(worker_mutex_);
            for (auto& w : workers_) w.request_stop();
            join.swap(workers_);
        }
        cv_.notify_all();
        join.clear();
        stats_.workers.store(0);
    }

    void worker(std::stop_token stop, uint32_t k, uint32_t n) {
        const int interval = env_int("GTA6RT_XMA_LOG_INTERVAL", 10);
        uint64_t next_summary = now_ns() + uint64_t(std::max(interval, 1)) * 1000000000ull;
        uint64_t seen = 0, last_frames = 0;
        while (!stop.stop_requested()) {
            pump_share(k, n);
            if (k == 0 && log_enabled() && now_ns() >= next_summary) {
                next_summary = now_ns() + uint64_t(std::max(interval, 1)) * 1000000000ull;
                if (stats_.frames.load() != last_frames || stats_.active.load()) {
                    last_frames = stats_.frames.load();
                    std::shared_lock table(table_);
                    summary("summary");
                }
            }
            std::unique_lock lock(worker_mutex_);
            if (wake_seq_ != seen) {
                seen = wake_seq_;
                continue;
            }
            if (stats_.active.load() > 0) {
                cv_.wait_for(lock, stop, std::chrono::milliseconds(2), [&] { return wake_seq_ != seen; });
            } else {
                cv_.wait(lock, stop, [&] { return wake_seq_ != seen; });
            }
            seen = wake_seq_;
        }
    }

    mutable std::shared_mutex table_;
    uint32_t* words_ = nullptr;
    uint32_t count_ = 0;
    gta6rt_xma_span_fn span_ = nullptr;
    void* user_ = nullptr;
    std::vector<std::unique_ptr<Slot>> slots_;
    std::vector<std::unique_ptr<Snapshot>> snaps_;
    std::vector<uint32_t> gens_;
    uint64_t bind_ns_ = 0;
    Stats stats_;

    std::mutex worker_mutex_;
    std::condition_variable_any cv_;
    std::vector<std::jthread> workers_;
    uint64_t wake_seq_ = 0;
};

Engine& engine() {
    static Engine* e = new Engine();
    return *e;
}

}  // namespace

extern "C" void gta6rt_xma_observe(uint32_t command, const void* config, const uint32_t* words, uint32_t count) {
    if (!log_enabled()) return;
    static std::atomic<uint64_t> seen[20];
    static std::atomic<uint64_t> first_ns{0}, next_summary{0};
    uint64_t zero = 0;
    const uint64_t now = now_ns();
    first_ns.compare_exchange_strong(zero, now);
    if (command >= 20) return;
    const uint64_t n = seen[command].fetch_add(1) + 1;
    uint32_t index = 0;
    if (config) std::memcpy(&index, config, 4);
    if (command == kCmdEnableXma && (n <= 64 || n % 1000 == 0)) {
        if (words && index < count) {
            const Ctx c{const_cast<uint32_t*>(words) + 16u * index};
            logf("%sENABLE ctx %u (silent path, no decode): %u Hz %u ch, in0 %u pkts @%08x, read bit %u, start skip %u "
                 "sub, loops %u [%u..%u], ring %u blocks, t=%.3f s",
                 n == 1 ? "first " : "", index, kRates[c.field(1, 27, 2)], 1u + c.field(1, 29, 1), c.field(0, 0, 12),
                 c.get(5), c.field(2, 0, 26), c.field(1, 24, 3), c.field(0, 12, 8), c.field(3, 0, 26),
                 c.field(4, 0, 26), c.field(0, 22, 5), (now - first_ns.load()) / 1e9);
        } else {
            logf("%sENABLE ctx %u (silent path, no decode), t=%.3f s", n == 1 ? "first " : "", index,
                 (now - first_ns.load()) / 1e9);
        }
    } else if (n == 1 && command != 2) {
        logf("first command %u (silent path), t=%.3f s", command, (now - first_ns.load()) / 1e9);
    }
    const int interval = std::max(env_int("GTA6RT_XMA_LOG_INTERVAL", 10), 1);
    uint64_t due = next_summary.load();
    if (now >= due && next_summary.compare_exchange_strong(due, now + uint64_t(interval) * 1000000000ull) && due) {
        logf("silent path summary: enable %llu disable %llu advance %llu enable-many %llu disable-many %llu",
             (unsigned long long)seen[kCmdEnableXma].load(), (unsigned long long)seen[kCmdDisableXma].load(),
             (unsigned long long)seen[kCmdAdvanceXmaRead].load(), (unsigned long long)seen[kCmdEnableXmas].load(),
             (unsigned long long)seen[kCmdDisableXmas].load());
    }
}

extern "C" int gta6rt_xma_wanted(void) {
    if (const char* x = std::getenv("GTA6RT_XMA"); x && *x) return std::string_view(x) == "1" ? 1 : 0;
    const char* a = std::getenv("GTA6RT_AUDIO");
    if (!a) return 0;
    const std::string_view m(a);
    return (m == "on" || m == "null") ? 1 : 0;
}

extern "C" int gta6rt_xma_bind(uint32_t* context_words, uint32_t count, gta6rt_xma_span_fn span, void* user) {
    if (!context_words || !span || count == 0) return -1;
    try {
        return engine().bind(context_words, count, span, user);
    } catch (const std::bad_alloc&) {
        logf("bind of %u contexts: out of memory", count);
        return -1;
    }
}

extern "C" void gta6rt_xma_unbind(void) { engine().unbind(); }

extern "C" int gta6rt_xma_command(uint32_t command, const void* config) {
    try {
        return engine().command(command, config);
    } catch (const std::exception& e) {
        logf("command %u: %s", command, e.what());
        return 1;
    }
}

extern "C" void gta6rt_xma_get_stats(struct gta6rt_xma_stats* out) {
    if (out) engine().stats(out);
}
