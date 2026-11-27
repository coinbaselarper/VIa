#pragma once

// port/: GPU memory shared by render targets the title places over the same physical memory. The title keeps its
// slot-133 targets on far fewer pages than their spans add up to (a console frame graph: targets that are never live
// at the same time share memory); each one used to get a private image. A target's title bytes are a list of runs of
// physical memory (VM object, offset); two targets whose runs overlap by at least a page (or half the smaller one)
// alias: the title never has both live. Aliasing targets may share bytes of one VkDeviceMemory (an extent; on MoltenVK
// a placement MTLHeap); targets that do not alias never do. An extent is laid out when the first of its targets is
// used, from every target registered by then; later ones go where they overlap only their aliases, else privately.
// The title treats an alias's contents as undefined once another is written; the use hook checks that it does.

#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace leonida::rt_alias {

enum Use : int { use_read = 0, use_write = 1, use_activate = 2, use_common = 3 };

struct Run {
    std::uint64_t object = 0, begin = 0, end = 0;
    bool operator==(const Run&) const = default;
};
using Runs = std::vector<Run>;

inline std::uint64_t overlap(const Runs& a, const Runs& b) {
    std::uint64_t n = 0;
    for (std::size_t i = 0, j = 0; i < a.size() && j < b.size();) {
        if (a[i].object != b[j].object) {
            (a[i].object < b[j].object ? i : j)++;
            continue;
        }
        const std::uint64_t lo = std::max(a[i].begin, b[j].begin), hi = std::min(a[i].end, b[j].end);
        if (lo < hi) n += hi - lo;
        (a[i].end < b[j].end ? i : j)++;
    }
    return n;
}

inline std::uint64_t bytes(const Runs& r) {
    std::uint64_t n = 0;
    for (const Run& x : r) n += x.end - x.begin;
    return n;
}

inline bool aliases(const Runs& a, const Runs& b) {
    const std::uint64_t shared = overlap(a, b);
    return shared && shared >= std::min<std::uint64_t>(65536, std::min(bytes(a), bytes(b)) / 2);
}

struct Extent;

struct Watch {
    std::atomic<Extent*> extent{nullptr};
    std::uint64_t lo = 0, hi = 0;
    char shape[48]{};
    bool clobbered = false, written = false;
    const Watch* clobbered_by = nullptr;
    std::vector<Watch*> overlaps;
};

struct Member {
    const void* owner;
    Runs runs;
    std::uint64_t lo, hi;
};

struct Extent {
    VkDevice device{};
    PFN_vkFreeMemory free_memory{};
    VkDeviceMemory memory{};
    std::uint32_t type = 0;
    std::uint64_t bytes = 0, title_bytes = 0;
    std::vector<Member> members;
    std::mutex mutex;
    std::vector<Watch*> watches;

    ~Extent() {
        if (memory) free_memory(device, memory, nullptr);
    }
};

struct Stats {
    std::uint64_t candidates = 0, candidate_bytes = 0, extents = 0, extent_bytes = 0, extent_title_bytes = 0,
                  members = 0, member_bytes = 0, late = 0;
    std::uint64_t refused[6]{}, refused_bytes[6]{};
    std::uint64_t uses[4]{}, hazards = 0, moved = 0, moved_before_bind = 0, split = 0, no_room = 0, no_room_bytes = 0;
};

inline const char* refusal_name(int r) {
    static const char* names[] = {"unmapped", "?", "memory type", "allocation", "bind", "?"};
    return names[r];
}

inline std::atomic<std::uint64_t> g_uses[4]{};
inline std::atomic<std::uint64_t> g_hazards{0};

inline void note_use(const void* opaque, int use) {
    auto* w = const_cast<Watch*>(static_cast<const Watch*>(opaque));
    Extent* x = w->extent.load(std::memory_order_acquire);
    if (!x || use < 0 || use > 3) return;
    g_uses[use].fetch_add(1, std::memory_order_relaxed);
    if (use == use_common) return;
    std::lock_guard lock(x->mutex);
    if (use == use_read) {
        if (!w->clobbered) return;
        const Watch* by = w->clobbered_by;
        w->clobbered = false;
        if (g_hazards.fetch_add(1, std::memory_order_relaxed) < 32)
            std::fprintf(stderr,
                         "[rt] rt-alias hazard: %s (extent +0x%llx..+0x%llx) read after %s (+0x%llx..+0x%llx) wrote "
                         "memory they share; it was not written or discarded since\n",
                         w->shape, (unsigned long long)w->lo, (unsigned long long)w->hi, by ? by->shape : "?",
                         by ? (unsigned long long)by->lo : 0ull, by ? (unsigned long long)by->hi : 0ull);
        return;
    }
    w->clobbered = false;
    w->written = true;
    for (Watch* o : w->overlaps) {
        o->clobbered = o->written;
        o->clobbered_by = w;
    }
}

class Pool {
public:
    struct Candidate {
        std::uint64_t cpu = 0, span = 0;
        Runs runs;
        VkMemoryRequirements req{};
        Watch* watch = nullptr;
        std::weak_ptr<Extent> extent;
    };

    static bool enabled() {
        static const bool on = [] {
            const char* v = std::getenv("LEONIDA_RT_ALIAS");
            return !v || std::strcmp(v, "0");
        }();
        return on;
    }

    void add(const void* owner, std::uint64_t cpu, std::uint64_t span, Runs runs, const VkMemoryRequirements& req,
             Watch* watch) {
        std::lock_guard lock(mutex_);
        auto& c = candidates_[owner];
        c.cpu = cpu, c.span = span, c.runs = std::move(runs), c.req = req, c.watch = watch;
        if (c.runs.size() > 1) ++split_;
    }

    void add_if_missing(const void* owner, std::uint64_t cpu, std::uint64_t span, const Runs& runs,
                        const VkMemoryRequirements& req, Watch* watch) {
        {
            std::lock_guard lock(mutex_);
            if (candidates_.count(owner)) return;
        }
        add(owner, cpu, span, runs, req, watch);
    }

    void remove(const void* owner) {
        std::lock_guard lock(mutex_);
        const auto it = candidates_.find(owner);
        if (it == candidates_.end()) return;
        if (Watch* w = it->second.watch) unlink(*w);
        if (auto x = it->second.extent.lock())
            std::erase_if(x->members, [&](const Member& m) { return m.owner == owner; });
        candidates_.erase(it);
    }

    void refuse(int reason, std::uint64_t size) {
        std::lock_guard lock(mutex_);
        ++refused_[reason];
        refused_bytes_[reason] += size;
    }

    // The extent and offset for `owner`'s image at its first use, or null (`why` set): a private allocation then.
    template <class Allocate>
    std::shared_ptr<Extent> place(const void* owner, const Runs& runs, Allocate&& allocate, VkDeviceSize& offset,
                                  int& why) {
        std::lock_guard lock(mutex_);
        const auto found = candidates_.find(owner);
        if (found == candidates_.end()) {
            why = 0;
            return {};
        }
        Candidate& c = found->second;
        if (c.runs != runs) {
            ++moved_;
            c.runs = runs;
        }
        const auto refuse = [&](int reason) {
            why = reason;
            ++refused_[reason];
            refused_bytes_[reason] += c.req.size;
            return std::shared_ptr<Extent>{};
        };
        std::vector<std::pair<std::uint64_t, std::shared_ptr<Extent>>> related;
        std::erase_if(extents_, [](const auto& w) { return w.expired(); });
        for (const auto& weak : extents_) {
            auto x = weak.lock();
            if (!x) continue;
            std::uint64_t shared = 0;
            for (const Member& m : x->members)
                if (aliases(m.runs, c.runs)) shared += overlap(m.runs, c.runs);
            if (shared) related.emplace_back(shared, std::move(x));
        }
        std::sort(related.begin(), related.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::shared_ptr<Extent> x;
        if (!related.empty()) {
            for (auto& [shared, candidate] : related) {
                if (!(c.req.memoryTypeBits & (1u << candidate->type))) continue;
                const auto at = fit(candidate->members, c.runs, c.req, candidate->bytes);
                if (at == ~0ull) continue;
                x = candidate;
                offset = at;
                break;
            }
            if (x) {
                x->members.push_back({owner, c.runs, offset, offset + c.req.size});
                ++late_;
            } else {
                ++no_room_;
                no_room_bytes_ += c.req.size;
            }
        }
        if (!x) {
            x = make_extent(owner, c, allocate);
            if (!x) return refuse(3);
            for (const Member& m : x->members)
                if (m.owner == owner) offset = m.lo;
        }
        c.extent = x;
        if (Watch* w = c.watch) link(*x, *w, offset, offset + c.req.size);
        return x;
    }

    void unplace(const void* owner) {
        std::lock_guard lock(mutex_);
        const auto it = candidates_.find(owner);
        if (it == candidates_.end()) return;
        if (Watch* w = it->second.watch) unlink(*w);
        if (auto x = it->second.extent.lock())
            std::erase_if(x->members, [&](const Member& m) { return m.owner == owner; });
        it->second.extent.reset();
        ++refused_[4];
        refused_bytes_[4] += it->second.req.size;
    }

    template <class Probe>
    Stats stats(Probe&& probe) {
        Stats s;
        std::lock_guard lock(mutex_);
        for (auto& [owner, c] : candidates_) {
            ++s.candidates;
            s.candidate_bytes += c.req.size;
            if (auto x = c.extent.lock()) {
                ++s.members;
                s.member_bytes += c.req.size;
                Runs now;
                if (!probe(c.cpu, c.span, now) || now != c.runs) ++s.moved;
            }
        }
        for (const auto& weak : extents_)
            if (auto x = weak.lock()) {
                ++s.extents;
                s.extent_bytes += x->bytes;
                s.extent_title_bytes += x->title_bytes;
            }
        s.late = late_;
        for (int i = 0; i < 6; ++i) s.refused[i] = refused_[i], s.refused_bytes[i] = refused_bytes_[i];
        for (int i = 0; i < 4; ++i) s.uses[i] = g_uses[i].load(std::memory_order_relaxed);
        s.hazards = g_hazards.load(std::memory_order_relaxed);
        s.moved_before_bind = moved_;
        s.split = split_;
        s.no_room = no_room_;
        s.no_room_bytes = no_room_bytes_;
        return s;
    }

private:
    static VkDeviceSize align(VkDeviceSize v, VkDeviceSize a) { return a > 1 ? (v + a - 1) / a * a : v; }

    // The lowest offset where an image of `req` overlaps only members it aliases, below `limit`; ~0 when none.
    static std::uint64_t fit(const std::vector<Member>& members, const Runs& runs, const VkMemoryRequirements& req,
                             std::uint64_t limit) {
        std::vector<const Member*> conflicts;
        for (const Member& m : members)
            if (!aliases(m.runs, runs)) conflicts.push_back(&m);
        std::vector<std::uint64_t> starts{0};
        for (const Member* m : conflicts) starts.push_back(align(m->hi, req.alignment));
        std::sort(starts.begin(), starts.end());
        for (const std::uint64_t at : starts) {
            if (at + req.size > limit) break;
            const bool clear = std::none_of(conflicts.begin(), conflicts.end(), [&](const Member* m) {
                return m->lo < at + req.size && at < m->hi;
            });
            if (clear) return at;
        }
        return ~0ull;
    }

    template <class Allocate>
    std::shared_ptr<Extent> make_extent(const void* seed_owner, const Candidate& seed, Allocate& allocate) {
        std::vector<std::pair<const void*, const Candidate*>> group{{seed_owner, &seed}};
        std::unordered_map<const Candidate*, bool> seen{{&seed, true}};
        std::uint32_t bits = seed.req.memoryTypeBits;
        for (std::size_t next = 0; next < group.size(); ++next) {
            const Candidate* g = group[next].second;
            for (auto& [owner, c] : candidates_) {
                if (!c.extent.expired() || !(c.req.memoryTypeBits & bits) || seen.count(&c)) continue;
                if (!aliases(c.runs, g->runs)) continue;
                seen[&c] = true;
                if (related_to_extent(c.runs)) continue;
                group.emplace_back(owner, &c);
                bits &= c.req.memoryTypeBits;
            }
        }
        std::sort(group.begin(), group.end(), [](const auto& a, const auto& b) {
            return a.second->req.size > b.second->req.size;
        });
        auto x = std::make_shared<Extent>();
        VkDeviceSize alignment = 1;
        std::uint64_t own = 0;
        Runs all;
        for (const auto& [owner, c] : group) {
            const std::uint64_t at = fit(x->members, c->runs, c->req, ~0ull >> 1);
            x->members.push_back({owner, c->runs, at, at + c->req.size});
            x->bytes = std::max(x->bytes, at + c->req.size);
            alignment = std::max<VkDeviceSize>(alignment, c->req.alignment);
            own += c->req.size;
            all.insert(all.end(), c->runs.begin(), c->runs.end());
        }
        x->bytes = align(x->bytes, alignment);
        x->title_bytes = distinct(all);
        if (group.size() > 8 && std::getenv("LEONIDA_RT_ALIAS_DUMP"))
            for (const auto& [owner, c] : group) {
                std::fprintf(stderr, "[rt] rt-alias dump %p size %llu align %llu runs", owner,
                             (unsigned long long)c->req.size, (unsigned long long)c->req.alignment);
                for (const Run& r : c->runs)
                    std::fprintf(stderr, " %llx:%llx-%llx", (unsigned long long)r.object, (unsigned long long)r.begin,
                                 (unsigned long long)r.end);
                std::fprintf(stderr, "\n");
            }
        x->memory = allocate(x->bytes, alignment, bits, x->type, x->device, x->free_memory);
        if (!x->memory) return {};
        extents_.push_back(x);
        if ((group.size() > 1 || x->bytes >= (4u << 20)) && logged_++ < 40)
            std::fprintf(stderr,
                         "[rt] rt-alias: extent of %zu render targets (%.2f MiB of images) over %.2f MiB of title "
                         "memory: %.2f MiB\n",
                         group.size(), double(own) / 1048576.0, double(x->title_bytes) / 1048576.0,
                         double(x->bytes) / 1048576.0);
        return x;
    }

    bool related_to_extent(const Runs& runs) const {
        for (const auto& weak : extents_)
            if (auto x = weak.lock())
                for (const Member& m : x->members)
                    if (aliases(m.runs, runs)) return true;
        return false;
    }

    static std::uint64_t distinct(Runs runs) {
        std::sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) {
            return a.object != b.object ? a.object < b.object : a.begin < b.begin;
        });
        std::uint64_t n = 0, object = 0, end = 0;
        bool any = false;
        for (const Run& r : runs) {
            if (!any || r.object != object || r.begin >= end) {
                n += r.end - r.begin;
                object = r.object, end = r.end, any = true;
            } else if (r.end > end) {
                n += r.end - end;
                end = r.end;
            }
        }
        return n;
    }

    static void link(Extent& x, Watch& w, std::uint64_t lo, std::uint64_t hi) {
        std::lock_guard lock(x.mutex);
        w.lo = lo, w.hi = hi;
        w.clobbered = w.written = false;
        w.overlaps.clear();
        for (Watch* o : x.watches)
            if (o->lo < hi && lo < o->hi) {
                o->overlaps.push_back(&w);
                w.overlaps.push_back(o);
            }
        x.watches.push_back(&w);
        w.extent.store(&x, std::memory_order_release);
    }

    static void unlink(Watch& w) {
        Extent* x = w.extent.exchange(nullptr, std::memory_order_acq_rel);
        if (!x) return;
        std::lock_guard lock(x->mutex);
        std::erase(x->watches, &w);
        for (Watch* o : w.overlaps) {
            std::erase(o->overlaps, &w);
            if (o->clobbered_by == &w) o->clobbered_by = nullptr;
        }
        w.overlaps.clear();
    }

    std::mutex mutex_;
    std::unordered_map<const void*, Candidate> candidates_;
    std::vector<std::weak_ptr<Extent>> extents_;
    std::uint64_t refused_[6]{}, refused_bytes_[6]{}, late_ = 0, moved_ = 0, split_ = 0, logged_ = 0, no_room_ = 0,
                  no_room_bytes_ = 0;
};

}  // namespace leonida::rt_alias
