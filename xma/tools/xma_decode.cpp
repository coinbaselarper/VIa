// Decodes one raw XMA2 stream (whole 2048-byte packets) the way the title plays it: through the context engine
// with the title's context setup (title_driver.h), or with --packets through the plain in-order packet decoder.
// Optionally compares against a reference s16 file (e.g. ffmpeg's decode) and prints one JSON line.
//
//   xma_decode IN.xma RATE CHANNELS [--out OUT.s16] [--packets] [--start S] [--loop LS LE --iterations N]
//              [--ref REF.s16 [--max-lag L] [--lag L]]
// With --loop, --ref and --lag the reference is the reference decode played the same way (start to loop end, then
// the loop region N times); "steady" leaves out the first frame after each jump (decoded with the loop-end frame's
// state, as the console does, instead of its natural predecessor's).
#include <gta6rt/xma.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "title_driver.h"

namespace {

struct EngineHal {
    void command(uint32_t cmd, const void* cfg) { gta6rt_xma_command(cmd, cfg); }
};

std::vector<uint8_t> read_file(const char* path) {
    std::vector<uint8_t> v;
    if (FILE* f = std::fopen(path, "rb")) {
        std::fseek(f, 0, SEEK_END);
        v.resize(static_cast<std::size_t>(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        if (std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
        std::fclose(f);
    }
    return v;
}

std::vector<int16_t> decode_packets(const std::vector<uint8_t>& in, uint32_t rate, uint32_t ch) {
    Gta6rtXmaDecoder* d = nullptr;
    if (gta6rt_xma_decoder_create(rate, ch, &d) != GTA6RT_XMA_OK) return {};
    std::vector<int16_t> out, tmp(64 * 512 * 2);
    for (std::size_t p = 0; p + 2048 <= in.size(); p += 2048) {
        uint32_t frames = 0;
        gta6rt_xma_decoder_packet(d, in.data() + p, tmp.data(), tmp.size(), &frames);
        out.insert(out.end(), tmp.begin(), tmp.begin() + frames * 512 * ch);
    }
    gta6rt_xma_decoder_destroy(d);
    return out;
}

std::vector<int16_t> decode_engine(const std::vector<uint8_t>& in, uint32_t rate, uint32_t ch, title::Buffer buf,
                                   std::size_t want, uint32_t* position) {
    title::Apu apu(in.size() + (title::kContexts * (title::kRingBytes + title::kWorkBytes)) + (1u << 20));
    std::vector<uint32_t> ctx(title::kContexts * 16);
    EngineHal hal;
    title::Title<EngineHal> t(hal, apu, ctx.data());
    gta6rt_xma_bind(ctx.data(), title::kContexts, &title::Apu::span, &apu);
    title::Wave w;
    w.apu = apu.alloc(static_cast<uint32_t>(in.size()));
    std::memcpy(apu.ptr(w.apu), in.data(), in.size());
    w.data = apu.ptr(w.apu);
    w.bytes = static_cast<uint32_t>(in.size());
    w.rate = rate;
    w.channels = ch;
    auto v = t.submit(w, buf);
    *position = v.base + v.position;
    t.enable(v);
    std::vector<int16_t> out;
    auto last = std::chrono::steady_clock::now();
    while (out.size() < want) {
        if (t.update(v, out)) last = std::chrono::steady_clock::now();
        else if (std::chrono::steady_clock::now() - last > std::chrono::milliseconds(300)) break;
        else std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
    t.release(v);
    gta6rt_xma_unbind();
    if (out.size() > want) out.resize(want);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: xma_decode IN.xma RATE CHANNELS [--out F] [--packets] [--start S] "
                             "[--loop LS LE --iterations N] [--ref F [--max-lag L]]\n");
        return 2;
    }
    const auto in = read_file(argv[1]);
    const uint32_t rate = static_cast<uint32_t>(std::atoi(argv[2]));
    const uint32_t ch = static_cast<uint32_t>(std::atoi(argv[3]));
    const char *out_path = nullptr, *ref_path = nullptr;
    bool packets = false;
    title::Buffer buf;
    int iterations = 0, max_lag = 2048, fixed_lag = 0;
    bool fixed_lag_set = false;
    for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) out_path = argv[++i];
        else if (a == "--packets") packets = true;
        else if (a == "--start" && i + 1 < argc) buf.start = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (a == "--loop" && i + 2 < argc) {
            buf.loop_start = static_cast<uint32_t>(std::atoi(argv[++i]));
            buf.loop_end = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (a == "--iterations" && i + 1 < argc) iterations = std::atoi(argv[++i]);
        else if (a == "--ref" && i + 1 < argc) ref_path = argv[++i];
        else if (a == "--max-lag" && i + 1 < argc) max_lag = std::atoi(argv[++i]);
        else if (a == "--lag" && i + 1 < argc) fixed_lag = std::atoi(argv[++i]), fixed_lag_set = true;
    }
    if (in.empty() || in.size() % 2048) {
        std::fprintf(stderr, "xma_decode: %s is not whole 2048-byte packets\n", argv[1]);
        return 2;
    }
    uint32_t total_frames = 0;
    for (std::size_t p = 0; p < in.size(); p += 2048) total_frames += in[p] >> 2;
    uint32_t position = 0;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<int16_t> out;
    if (packets) {
        out = decode_packets(in, rate, ch);
    } else {
        std::size_t want = std::size_t(total_frames) * 512 * ch;
        if (buf.loop_end) {
            const uint32_t rs = (buf.loop_start >> 9) * 512 + ((buf.loop_start >> 7) & 3) * 128;
            const uint32_t re = (buf.loop_end >> 9) * 512 + (((buf.loop_end >> 7) & 3) + 1) * 128;
            want = (std::size_t(re) + std::size_t(std::max(iterations, 0)) * (re - rs)) * ch;
        }
        out = decode_engine(in, rate, ch, buf, want, &position);
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (out_path) {
        if (FILE* f = std::fopen(out_path, "wb")) {
            std::fwrite(out.data(), 2, out.size(), f);
            std::fclose(f);
        }
    }
    std::printf("{\"frames\":%u,\"samples\":%zu,\"position\":%u,\"wall_s\":%.4f", total_frames, out.size() / ch,
                position, secs);
    if (ref_path && buf.loop_end && fixed_lag_set) {
        const auto rb = read_file(ref_path);
        std::vector<int16_t> ref(rb.size() / 2);
        std::memcpy(ref.data(), rb.data(), ref.size() * 2);
        const uint32_t rs = (buf.loop_start >> 9) * 512 + ((buf.loop_start >> 7) & 3) * 128;
        const uint32_t re = (buf.loop_end >> 9) * 512 + (((buf.loop_end >> 7) & 3) + 1) * 128;
        auto at = [&](long j) -> int {
            const long r = (j + fixed_lag) * long(ch);
            return (r >= 0 && std::size_t(r) < ref.size()) ? ref[std::size_t(r)] : 0;
        };
        auto has = [&](long j) {
            const long r = (j + fixed_lag) * long(ch);
            return r >= 0 && std::size_t(r) < ref.size();
        };
        std::vector<int16_t> expect;
        std::vector<uint8_t> steady, covered;
        for (uint32_t j = 0; j < re; ++j) expect.push_back(at(j)), steady.push_back(1), covered.push_back(has(j));
        for (int it = 0; it < iterations; ++it) {
            for (uint32_t j = rs; j < re; ++j) {
                expect.push_back(at(j));
                steady.push_back(j >= ((rs & ~511u) + 512));
                covered.push_back(has(j));
            }
        }
        double se_all = 0, se_steady = 0, sxy = 0, sxx = 0, syy = 0;
        std::size_t n_all = 0, n_steady = 0, eq_steady = 0;
        for (std::size_t k = 0; k < out.size() / ch && k < expect.size(); ++k) {
            if (!covered[k]) continue;
            const double x = out[k * ch], y = expect[k];
            se_all += (x - y) * (x - y), ++n_all, sxy += x * y, sxx += x * x, syy += y * y;
            if (steady[k]) se_steady += (x - y) * (x - y), ++n_steady, eq_steady += (x == y);
        }
        auto psnr = [](double se, std::size_t n) {
            return (n && se > 0) ? 10.0 * std::log10(32767.0 * 32767.0 * double(n) / se) : 999.0;
        };
        std::printf(",\"loop_region\":[%u,%u],\"iterations\":%d,\"expected\":%zu,\"compared\":%zu,"
                    "\"corr\":%.9f,\"psnr_all_db\":%.2f,\"psnr_steady_db\":%.2f,\"steady\":%zu,"
                    "\"steady_equal\":%zu",
                    rs, re, iterations, expect.size(), n_all, (sxx > 0 && syy > 0) ? sxy / std::sqrt(sxx * syy) : 1.0,
                    psnr(se_all, n_all), psnr(se_steady, n_steady), n_steady, eq_steady);
    } else if (ref_path) {
        const auto rb = read_file(ref_path);
        std::vector<int16_t> ref(rb.size() / 2);
        std::memcpy(ref.data(), rb.data(), ref.size() * 2);
        const long shift = long(position) * long(ch);
        auto stats = [&](int lag, std::size_t from, std::size_t to, double* corr, double* mse) {
            double sxy = 0, sxx = 0, syy = 0, se = 0;
            std::size_t n = 0;
            for (std::size_t k = from; k < to && k < out.size(); ++k) {
                const long r = long(k) + shift + lag * long(ch);
                if (r < 0 || std::size_t(r) >= ref.size()) continue;
                const double x = out[k], y = ref[std::size_t(r)];
                sxy += x * y, sxx += x * x, syy += y * y, se += (x - y) * (x - y);
                ++n;
            }
            *corr = (sxx > 0 && syy > 0) ? sxy / std::sqrt(sxx * syy) : (se == 0 ? 1.0 : 0.0);
            *mse = n ? se / double(n) : 0;
            return n;
        };
        double best = -2, corr = 0, mse = 0;
        int best_lag = 0;
        const std::size_t win_from = std::min<std::size_t>(out.size() / 4, 4096 * ch);
        const std::size_t win_to = std::min(out.size(), win_from + 65536 * ch);
        if (fixed_lag_set) {
            best_lag = fixed_lag;
        } else {
            for (int lag = -max_lag; lag <= max_lag; ++lag) {
                if (stats(lag, win_from, win_to, &corr, &mse) && corr > best) best = corr, best_lag = lag;
            }
        }
        double best_mse = 0, steady_corr = 0, steady_mse = 0;
        const std::size_t best_n = stats(best_lag, 0, out.size(), &best, &best_mse);
        const std::size_t loose = position ? (512 - (position & 511)) * ch : 0;
        const std::size_t steady_n = stats(best_lag, loose, out.size(), &steady_corr, &steady_mse);
        const double psnr = best_mse > 0 ? 10.0 * std::log10(32767.0 * 32767.0 / best_mse) : 999.0;
        std::size_t equal = 0, maxdiff = 0;
        for (std::size_t k = loose; k < out.size(); ++k) {
            const long r = long(k) + shift + best_lag * long(ch);
            if (r < 0 || std::size_t(r) >= ref.size()) continue;
            const long d = std::labs(long(out[k]) - long(ref[std::size_t(r)]));
            equal += d == 0;
            maxdiff = std::max<std::size_t>(maxdiff, std::size_t(d));
        }
        const double steady_psnr = steady_mse > 0 ? 10.0 * std::log10(32767.0 * 32767.0 / steady_mse) : 999.0;
        std::printf(",\"ref_samples\":%zu,\"lag\":%d,\"compared\":%zu,\"corr\":%.9f,\"psnr_db\":%.2f,"
                    "\"steady\":%zu,\"steady_corr\":%.9f,\"psnr_steady_db\":%.2f,\"steady_equal\":%zu,"
                    "\"max_abs_diff_steady\":%zu",
                    ref.size() / ch, best_lag, best_n, best, psnr, steady_n, steady_corr, steady_psnr, equal, maxdiff);
    }
    std::printf("}\n");
    return 0;
}
