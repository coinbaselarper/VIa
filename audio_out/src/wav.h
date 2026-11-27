#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace gta6rt::audio_out {

// 32-bit float WAV; the header is rewritten after every append so the file stays valid if the process is killed.
class WavWriter {
public:
    ~WavWriter() { close(); }
    bool open(const std::string& path, uint32_t rate, uint32_t channels);
    void append(const float* frames, uint32_t count);
    void close();
    bool is_open() const { return f_ != nullptr; }
    uint64_t frames() const { return frames_; }
    uint32_t channels() const { return channels_; }

private:
    void header();
    FILE* f_ = nullptr;
    uint32_t rate_ = 0, channels_ = 0;
    uint64_t frames_ = 0;
};

struct WavData {
    uint32_t rate = 0, channels = 0;
    std::vector<float> samples;
};

// Reads 16-bit PCM or 32-bit float WAV.
bool read_wav(const std::string& path, WavData* out);

}  // namespace gta6rt::audio_out
