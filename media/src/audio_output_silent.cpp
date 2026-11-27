#include "audio_output.h"
#include "media_common.h"

#include <mach/mach_time.h>

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace rt::xa2 {
namespace {

class SilentOutput final : public AudioOutput {
public:
    bool start(uint32_t rate, uint32_t channels, uint32_t quantum) override {
        rate_ = rate;
        quantum_ = quantum;
        (void)channels;
        due_ns_ = media::now_ns();
        quanta_ = 0;
        stopping_ = false;
        return true;
    }
    void stop() override {
        std::lock_guard lock(mu_);
        stopping_ = true;
        cv_.notify_all();
    }
    bool wait_ready() override {
        const uint64_t due = due_ns_ + (quanta_ * quantum_ * 1000000000ull) / rate_;
        std::unique_lock lock(mu_);
        while (!stopping_) {
            const uint64_t now = media::now_ns();
            if (now >= due) break;
            cv_.wait_for(lock, std::chrono::nanoseconds(due - now));
        }
        return !stopping_;
    }
    void write(const float*, uint32_t frames) override { quanta_ += frames / quantum_; }
    uint32_t latency_frames() const override { return 0; }
    uint32_t glitches() const override { return 0; }
    const char* name() const override { return "silent"; }

private:
    uint32_t rate_ = 48000, quantum_ = 480;
    uint64_t due_ns_ = 0, quanta_ = 0;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stopping_ = false;
};

}  // namespace

std::unique_ptr<AudioOutput> make_silent_output() { return std::make_unique<SilentOutput>(); }

}  // namespace rt::xa2
