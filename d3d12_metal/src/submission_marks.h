#pragma once
// What each queue has handed to the GPU, as progress-fence marks, for retiring objects submissions may still use.
//
// A queue replays a submission (reading the bindless table generation and title textures the context names) and
// encodes it before its progress value exists: an encode can wait seconds for a pipeline compiling at first use.
// Marks taken meanwhile must cover that submission, or what it recorded is freed under it (a destroyed descriptor
// heap's mutex in residency_for_submit, run vfx-t10). So a queue announces the value its submission will signal
// before it reads anything (begin_in_flight) and withdraws it once the value is signalled or the submission failed.

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace leonida {

template <class Fence>
class SubmissionMarks {
public:
    using Marks = std::vector<std::pair<Fence, std::uint64_t>>;

    void note_submitted(Fence fence, std::uint64_t value) {
        std::lock_guard lock(mutex_);
        auto& v = submitted_[fence];
        v = std::max(v, value);
    }
    void begin_in_flight(Fence fence, std::uint64_t value) {
        std::lock_guard lock(mutex_);
        in_flight_[fence] = value;
    }
    void end_in_flight(Fence fence) {
        std::lock_guard lock(mutex_);
        in_flight_.erase(fence);
    }
    void forget(Fence fence) {
        std::lock_guard lock(mutex_);
        submitted_.erase(fence);
        in_flight_.erase(fence);
    }
    // Signalled values and the values in-flight submissions will signal, per queue.
    Marks submitted() const {
        std::lock_guard lock(mutex_);
        std::map<Fence, std::uint64_t> all = submitted_;
        for (const auto& [fence, value] : in_flight_) {
            auto& v = all[fence];
            v = std::max(v, value);
        }
        return {all.begin(), all.end()};
    }
    // Signalled values only (what a present may wait for on the GPU).
    Marks submitted_only() const {
        std::lock_guard lock(mutex_);
        return {submitted_.begin(), submitted_.end()};
    }
    // Every queue of `marks` that still exists has completed its value (a queue that went counts as done).
    bool reached(const Marks& marks) const {
        std::lock_guard lock(mutex_);
        for (const auto& [fence, value] : marks)
            if ((submitted_.count(fence) || in_flight_.count(fence)) && fence->completed_value() < value) return false;
        return true;
    }

    // An object left at `left` with `marks` (taken then) may go once the queues passed those marks and, at least
    // `settle` later, the marks of that moment (taken on the first call past it into `settle_marks`).
    template <class TimePoint>
    bool passed(const Marks& marks, TimePoint left, TimePoint now, bool& settled, Marks& settle_marks,
                std::chrono::milliseconds settle = std::chrono::milliseconds(100)) const {
        if (!settled) {
            if (now - left < settle) return false;
            settle_marks = submitted();
            settled = true;
        }
        return reached(marks) && reached(settle_marks);
    }

private:
    mutable std::mutex mutex_;
    std::map<Fence, std::uint64_t> submitted_, in_flight_;
};

}  // namespace leonida
