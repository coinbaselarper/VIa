// The two pure-C++ sections of leonida's runtime/src/title_page_watch.h that xg_xs.cpp feeds and the D3D12 layer reads:
// texel-write ranges (4) and the layout word of every texture computer by shape (5). Copied verbatim (GPL-3.0-or-later);
// the other sections of that header are Windows page-protection code and belong to the D3D12 layer's port (S1.6).
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <tuple>
#include <utility>
#include <vector>

namespace leonida::title_texel_writes {

struct State {
    std::mutex mutex;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> ranges;  // [begin, end)
    bool overflow = false;                                          // too many to keep: check everything
    std::atomic<bool> pending{false};
};

inline State& state() {
    static State s;
    return s;
}

inline void note(const void* begin, std::size_t bytes) {
    if (!begin || !bytes) return;
    State& s = state();
    std::lock_guard lock(s.mutex);
    const auto at = reinterpret_cast<std::uintptr_t>(begin);
    if (s.ranges.size() < 4096) s.ranges.emplace_back(at, at + bytes);
    else s.overflow = true;
    s.pending.store(true, std::memory_order_release);
}

// The ranges noted since the last call; `overflow` says some were dropped.
inline std::vector<std::pair<std::uintptr_t, std::uintptr_t>> take(bool& overflow) {
    State& s = state();
    overflow = false;
    if (!s.pending.load(std::memory_order_acquire)) return {};
    std::lock_guard lock(s.mutex);
    overflow = std::exchange(s.overflow, false);
    s.pending.store(false, std::memory_order_relaxed);
    return std::exchange(s.ranges, {});
}

}  // namespace leonida::title_texel_writes

namespace leonida::title_xg_layouts {

struct Shape {
    std::uint32_t dimension, format, height, depth, mips;
    std::uint64_t width;
    bool operator<(const Shape& o) const {
        return std::tie(dimension, format, width, height, depth, mips) <
               std::tie(o.dimension, o.format, o.width, o.height, o.depth, o.mips);
    }
};

struct State {
    std::mutex mutex;
    std::map<Shape, std::uint32_t> layouts;
};

inline State& state() {
    static State s;
    return s;
}

inline void note(const Shape& shape, std::uint32_t layout) {
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (s.layouts.size() < 65536 || s.layouts.count(shape)) s.layouts[shape] = layout;
}

// The Layout word of the latest computer of this shape, or false.
inline bool find(const Shape& shape, std::uint32_t& layout) {
    State& s = state();
    std::lock_guard lock(s.mutex);
    const auto it = s.layouts.find(shape);
    if (it == s.layouts.end()) return false;
    layout = it->second;
    return true;
}

}  // namespace leonida::title_xg_layouts
