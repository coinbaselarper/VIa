#include "wav.h"

#include <cstring>

namespace gta6rt::audio_out {

namespace {
void put32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}
void put16(uint8_t* p, uint16_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
}
uint32_t get32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
}  // namespace

bool WavWriter::open(const std::string& path, uint32_t rate, uint32_t channels) {
    close();
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) return false;
    rate_ = rate;
    channels_ = channels;
    frames_ = 0;
    header();
    return true;
}

void WavWriter::header() {
    uint8_t h[44];
    const uint64_t bytes64 = frames_ * channels_ * 4;
    const uint32_t bytes = bytes64 > 0xFFFFFFF0ull ? 0xFFFFFFF0u : static_cast<uint32_t>(bytes64);
    std::memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + bytes);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 3);
    put16(h + 22, static_cast<uint16_t>(channels_));
    put32(h + 24, rate_);
    put32(h + 28, rate_ * channels_ * 4);
    put16(h + 32, static_cast<uint16_t>(channels_ * 4));
    put16(h + 34, 32);
    std::memcpy(h + 36, "data", 4);
    put32(h + 40, bytes);
    const long at = std::ftell(f_);
    std::fseek(f_, 0, SEEK_SET);
    std::fwrite(h, 1, sizeof h, f_);
    std::fseek(f_, at > 44 ? at : 44, SEEK_SET);
}

void WavWriter::append(const float* frames, uint32_t count) {
    if (!f_ || count == 0) return;
    std::fwrite(frames, sizeof(float) * channels_, count, f_);
    frames_ += count;
    header();
    std::fflush(f_);
}

void WavWriter::close() {
    if (!f_) return;
    header();
    std::fclose(f_);
    f_ = nullptr;
}

bool read_wav(const std::string& path, WavData* out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> b;
    uint8_t buf[1 << 16];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
    std::fclose(f);
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) return false;
    uint16_t fmt = 0, bits = 0;
    size_t pos = 12;
    while (pos + 8 <= b.size()) {
        const uint32_t len = get32(&b[pos + 4]);
        const uint8_t* body = &b[pos + 8];
        const size_t avail = b.size() - pos - 8;
        if (!std::memcmp(&b[pos], "fmt ", 4) && len >= 16) {
            fmt = get16(body);
            out->channels = get16(body + 2);
            out->rate = get32(body + 4);
            bits = get16(body + 14);
            if (fmt == 0xFFFE && len >= 40) fmt = get16(body + 24);
        } else if (!std::memcmp(&b[pos], "data", 4)) {
            const size_t bytes = len < avail ? len : avail;
            if (fmt == 3 && bits == 32) {
                out->samples.resize(bytes / 4);
                std::memcpy(out->samples.data(), body, out->samples.size() * 4);
            } else if (fmt == 1 && bits == 16) {
                out->samples.resize(bytes / 2);
                for (size_t i = 0; i < out->samples.size(); ++i)
                    out->samples[i] = static_cast<int16_t>(get16(body + 2 * i)) / 32768.0f;
            } else {
                return false;
            }
            return out->channels > 0;
        }
        pos += 8 + len + (len & 1);
    }
    return false;
}

}  // namespace gta6rt::audio_out
