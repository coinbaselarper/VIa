#pragma once

// port/d3d12_metal: render targets the title keeps on the same physical memory share one placement MTLHeap (the
// Vulkan path's rt_alias.h, d3d12_vulkan/src, for Metal). The title's slot-133 targets sit on far fewer pages than
// their spans add up to: a console frame graph whose targets are never live at the same time. A target's title bytes
// resolve to runs of (VM object, offset); two targets whose runs share half the smaller one's bytes alias (aliases()):
// the console cannot keep both, so the title never has both live. Targets under 256 KiB stay private (Pool::add).
// Aliases may share bytes of one extent (a
// placement heap); targets that do not alias never do. An extent is laid out at the first use of its first target
// from every target registered by then; later ones go where they overlap only their aliases, else into an extent of
// their own. Freed members stay in the layout (tombstones) so a non-alias never lands on memory whose GPU work may
// still be in flight. Lifetimes are the title's own; the replay hook checks them: a target used (read, loaded,
// drawn into) while its bytes are not its own (an alias wrote since its last write or clear) is counted, and with
// GTA6RT_METAL_RT_ALIAS_POISON=1 it is cleared to magenta (depth 0.5, stencil 0x5a) first, so a wrong assumption is
// visible in the frame. On by default; GTA6RT_METAL_RT_ALIAS=0 gives private textures, =census only registers and
// counts.

#include "metal_ledger.h"

#include <gta6rt_metal/gpu.h>

#pragma push_macro("__reserved")
#undef __reserved
#include <mach/mach.h>
#include <mach/mach_vm.h>
#pragma pop_macro("__reserved")

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

namespace leonida::metal_rt_alias {

namespace gpu = gta6rt::gpu;

inline int mode() {
    static const int m = [] {
        const char* v = std::getenv("GTA6RT_METAL_RT_ALIAS");
        if (!v || !*v) return 2;
        if (!std::strcmp(v, "0")) return 0;
        return std::strcmp(v, "census") ? 2 : 1;
    }();
    return m;
}
inline bool enabled() { return mode() == 2; }
inline bool watching() { return mode() != 0; }
inline bool poison() {
    static const bool on = [] {
        const char* v = std::getenv("GTA6RT_METAL_RT_ALIAS_POISON");
        return v && *v == '1';
    }();
    return on && enabled();
}

struct Run {
    std::uint64_t object = 0, begin = 0, end = 0;
    bool operator==(const Run&) const = default;
};
using Runs = std::vector<Run>;

// The VM-object runs behind [address, address + size), sorted and merged; false when a page is not mapped. xmem maps
// one 64 KiB pool slot at every window page that shows it, so shared object pages are shared physical memory.
inline bool title_runs(std::uint64_t address, std::uint64_t size, Runs& out) {
    constexpr std::uint64_t small = 4096, slot = 65536;
    out.clear();
    if (!address || !size) return false;
    const std::uint64_t end = address + size;
    for (std::uint64_t va = address; va < end;) {
        const std::uint64_t next = std::min(end, (va + slot) & ~(slot - 1));
        vm_page_info_basic_data_t info{};
        mach_msg_type_number_t count = VM_PAGE_INFO_BASIC_COUNT;
        if (mach_vm_page_info(mach_task_self(), va & ~(small - 1), VM_PAGE_INFO_BASIC,
                              reinterpret_cast<vm_page_info_t>(&info), &count) != KERN_SUCCESS ||
            !info.object_id)
            return false;
        const std::uint64_t begin = std::uint64_t(info.offset) + (va & (small - 1));
        if (!out.empty() && out.back().object == info.object_id && out.back().end == begin)
            out.back().end = begin + (next - va);
        else
            out.push_back({info.object_id, begin, begin + (next - va)});
        va = next;
    }
    std::sort(out.begin(), out.end(), [](const Run& a, const Run& b) {
        return a.object != b.object ? a.object < b.object : a.begin < b.begin;
    });
    std::size_t kept = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (kept && out[kept - 1].object == out[i].object && out[kept - 1].end >= out[i].begin)
            out[kept - 1].end = std::max(out[kept - 1].end, out[i].end);
        else
            out[kept++] = out[i];
    }
    out.resize(kept);
    return true;
}

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

// GTA6RT_METAL_RT_ALIAS_SHARE_PCT=<p> (50 when unset; 0: the old rule, 64 KiB or half the smaller): two targets alias
// only when they share p% of the smaller one's title bytes. A partial overlap on the console (the tail of one target on
// the head of another) becomes a full one in the placement heap, where fit() starts aliases at the same offset.
inline bool aliases(const Runs& a, const Runs& b) {
    static const std::uint64_t pct = [] {
        const char* v = std::getenv("GTA6RT_METAL_RT_ALIAS_SHARE_PCT");
        return v && *v ? std::strtoull(v, nullptr, 10) : 50ull;
    }();
    const std::uint64_t shared = overlap(a, b);
    if (!shared) return false;
    const std::uint64_t smaller = std::min(bytes(a), bytes(b));
    if (pct) return shared * 100 >= smaller * pct;
    return shared >= std::min<std::uint64_t>(65536, smaller / 2);
}

enum Use : int { use_read = 0, use_write = 1, use_fresh = 2, use_common = 3, use_wrote = 4 };

struct Extent;

struct Watch {
    std::atomic<Extent*> extent{nullptr};
    std::uint64_t lo = 0, hi = 0;
    char shape[64]{};
    bool valid = false, ever = false;
    const Watch* clobbered_by = nullptr;
    std::vector<Watch*> overlaps;
    std::atomic<std::uint64_t> reads{0}, writes{0};
};

struct Counters {
    std::atomic<std::uint64_t> uses[5]{};
    std::atomic<std::uint64_t> reads_after_handover{0}, reads_before_write{0}, loads_after_handover{0},
        loads_before_write{0}, poisoned{0}, zeroed{0}, fresh_handovers{0};
};
inline Counters& counters() {
    static Counters c;
    return c;
}

struct Member {
    const void* owner = nullptr;
    Runs runs;
    std::uint64_t lo = 0, hi = 0;
    bool live = true;
};

struct Extent {
    std::unique_ptr<gpu::Heap> heap;
    std::uint64_t bytes = 0, title_bytes = 0;
    std::vector<Member> members;
    std::mutex mutex;
    std::vector<Watch*> watches;
    ~Extent() {
        if (heap) metal_ledger::remove(metal_ledger::render_target, bytes);
    }
};

enum Action : int { act_none = 0, act_poison = 1, act_zero = 2 };

// What the caller clears the target to before the command: poison when its bytes are not its own (an alias wrote, or
// nothing did since it was placed) and the use is not one that starts its contents over; zero at its first use
// otherwise, as a committed target starts. use_wrote: a barrier out of a write state says the target was written
// (compute writes through descriptors are not seen otherwise).
inline int note_use(const void* opaque, int use) {
    auto* w = const_cast<Watch*>(static_cast<const Watch*>(opaque));
    if (use == use_read) w->reads.fetch_add(1, std::memory_order_relaxed);
    else if (use != use_common) w->writes.fetch_add(1, std::memory_order_relaxed);
    Extent* x = w->extent.load(std::memory_order_acquire);
    if (!x || use < 0 || use > 4) return act_none;
    auto& c = counters();
    c.uses[use].fetch_add(1, std::memory_order_relaxed);
    if (use == use_common) return act_none;
    std::lock_guard lock(x->mutex);
    int action = act_none;
    const bool handover = w->clobbered_by != nullptr;
    if (use == use_fresh) {
        if (!w->valid && handover) c.fresh_handovers.fetch_add(1, std::memory_order_relaxed);
    } else if (use == use_wrote) {
    } else if (!w->valid) {
        if (use == use_read) {
            const auto n = (handover ? c.reads_after_handover : c.reads_before_write).fetch_add(1, std::memory_order_relaxed);
            if (n < 16)
                std::fprintf(stderr,
                             "[rt] rt-alias hazard: %s (+0x%llx..+0x%llx) read %s%s; not written, cleared or discarded "
                             "since\n",
                             w->shape, (unsigned long long)w->lo, (unsigned long long)w->hi,
                             handover ? "after " : "before any write", handover ? w->clobbered_by->shape : "");
        } else {
            const auto n = (handover ? c.loads_after_handover : c.loads_before_write).fetch_add(1, std::memory_order_relaxed);
            if (n < 16)
                std::fprintf(stderr, "[rt] rt-alias hazard: %s (+0x%llx..+0x%llx) loaded %s%s; not written, cleared or "
                                     "discarded since\n",
                             w->shape, (unsigned long long)w->lo, (unsigned long long)w->hi,
                             handover ? "after " : "before any write", handover ? w->clobbered_by->shape : "");
        }
        action = poison() ? act_poison : !w->ever ? act_zero : act_none;
        if (action == act_poison) c.poisoned.fetch_add(1, std::memory_order_relaxed);
        if (action == act_zero) c.zeroed.fetch_add(1, std::memory_order_relaxed);
    }
    w->valid = w->ever = true;
    w->clobbered_by = nullptr;
    if (use != use_read)
        for (Watch* o : w->overlaps) {
            o->valid = false;
            o->clobbered_by = w;
        }
    return action;
}

struct Stats {
    std::uint64_t candidates = 0, candidate_bytes = 0, unmapped = 0, split = 0, placed = 0, placed_bytes = 0,
                  extents = 0, extent_bytes = 0, extent_title_bytes = 0, late = 0, no_room = 0, no_room_bytes = 0,
                  refused = 0, refused_bytes = 0, moved = 0, tombstones = 0, depth = 0, depth_bytes = 0, msaa = 0,
                  msaa_bytes = 0, twins = 0, twin_bytes = 0, unread_depth_msaa = 0, unread_depth_msaa_bytes = 0,
                  groups = 0, estimate_bytes = 0, used = 0, used_bytes = 0;
};

class Pool {
public:
    struct Candidate {
        std::uint64_t cpu = 0, span = 0, size = 0, alignment = 1;
        Runs runs;
        Watch* watch = nullptr;
        bool depth = false, msaa = false, mapped = false;
        std::uint32_t width = 0, height = 0, layers = 0, mips = 0, format = 0;
        std::weak_ptr<Extent> extent;
        std::uint64_t offset = 0;
        bool reserved = false, placed = false;
    };

    void add(const void* owner, Candidate c) {
        // GTA6RT_METAL_RT_ALIAS_MIN_KB=<n> (256 when unset; 0: every target aliases): a target under n KiB stays private. fit() puts aliases at the lowest
        // offset, so a small target the title keeps on the tail of a big one's pages lands on that one's first bytes
        // here (measured: the 1x1 exposure target, clobbered between PS_ResetExposure and PS_CompositeNMB).
        static const std::uint64_t min_bytes = [] {
            const char* v = std::getenv("GTA6RT_METAL_RT_ALIAS_MIN_KB");
            return (v && *v ? std::strtoull(v, nullptr, 10) : 256ull) << 10;
        }();
        if (c.size < min_bytes) {
            static std::atomic<int> told{0};
            if (told.fetch_add(1, std::memory_order_relaxed) < 12)
                std::fprintf(stderr, "[rt] rt-alias: %s (%llu KiB) under GTA6RT_METAL_RT_ALIAS_MIN_KB: private\n",
                             c.watch ? c.watch->shape : "?", static_cast<unsigned long long>(c.size >> 10));
            return;
        }
        Runs runs;
        c.mapped = title_runs(c.cpu, c.span, runs);
        c.runs = std::move(runs);
        std::lock_guard lock(mutex_);
        if (c.runs.size() > 1) ++split_;
        candidates_[owner] = std::move(c);
    }

    void remove(const void* owner) {
        std::lock_guard lock(mutex_);
        const auto it = candidates_.find(owner);
        if (it == candidates_.end()) return;
        if (Watch* w = it->second.watch) unlink(*w);
        if (auto x = it->second.extent.lock())
            for (Member& m : x->members)
                if (m.owner == owner) m.live = false, m.owner = nullptr;
        candidates_.erase(it);
    }

    // The extent and offset for `owner` at its first use, or null: a private texture then. `allocate(bytes)` makes the
    // placement heap.
    template <class Allocate>
    std::shared_ptr<Extent> place(const void* owner, Allocate&& allocate, std::uint64_t& offset) {
        std::lock_guard lock(mutex_);
        const auto found = candidates_.find(owner);
        if (found == candidates_.end()) return {};
        Candidate& c = found->second;
        Runs runs;
        if (!title_runs(c.cpu, c.span, runs)) {
            ++refused_, refused_bytes_ += c.size;
            return {};
        }
        if (runs != c.runs) {
            ++moved_;
            c.runs = std::move(runs);
            if (auto x = c.extent.lock(); x && c.reserved)
                std::erase_if(x->members, [&](const Member& m) { return m.owner == owner; });
            c.extent.reset();
            c.reserved = false;
        }
        std::shared_ptr<Extent> x = c.reserved ? c.extent.lock() : nullptr;
        if (x) {
            offset = c.offset;
        } else {
            c.reserved = false;
            x = late_fit(owner, c, offset);
            if (!x) {
                x = make_extent(owner, c, allocate);
                if (!x) {
                    ++refused_, refused_bytes_ += c.size;
                    return {};
                }
                offset = c.offset;
            }
        }
        c.placed = true;
        if (c.watch) link(*x, *c.watch, offset, offset + c.size);
        return x;
    }

    // The placed texture could not be made: forget the placement (the reservation stays as a tombstone).
    void unplace(const void* owner) {
        std::lock_guard lock(mutex_);
        const auto it = candidates_.find(owner);
        if (it == candidates_.end()) return;
        if (Watch* w = it->second.watch) unlink(*w);
        if (auto x = it->second.extent.lock())
            for (Member& m : x->members)
                if (m.owner == owner) m.live = false, m.owner = nullptr;
        it->second.extent.reset();
        it->second.placed = it->second.reserved = false;
        ++refused_, refused_bytes_ += it->second.size;
    }

    Stats stats() {
        Stats s;
        std::lock_guard lock(mutex_);
        std::vector<const Candidate*> live;
        for (auto& [owner, c] : candidates_) {
            ++s.candidates;
            s.candidate_bytes += c.size;
            if (!c.mapped) ++s.unmapped;
            if (c.depth) ++s.depth, s.depth_bytes += c.size;
            if (c.msaa) ++s.msaa, s.msaa_bytes += c.size;
            if ((c.depth || c.msaa) && c.watch && !c.watch->reads.load(std::memory_order_relaxed))
                ++s.unread_depth_msaa, s.unread_depth_msaa_bytes += c.size;
            if (c.placed) ++s.placed, s.placed_bytes += c.size;
            if (c.watch && (c.watch->reads.load(std::memory_order_relaxed) || c.watch->writes.load(std::memory_order_relaxed)))
                ++s.used, s.used_bytes += c.size;
            if (c.mapped && !c.runs.empty()) live.push_back(&c);
        }
        for (std::size_t i = 0; i < live.size(); ++i)
            for (std::size_t j = 0; j < i; ++j)
                if (live[i]->runs == live[j]->runs && live[i]->width == live[j]->width &&
                    live[i]->height == live[j]->height && live[i]->layers == live[j]->layers &&
                    live[i]->mips == live[j]->mips && live[i]->format != live[j]->format) {
                    ++s.twins, s.twin_bytes += live[i]->size;
                    break;
                }
        estimate(live, s);
        for (const auto& weak : extents_)
            if (auto x = weak.lock()) {
                ++s.extents;
                s.extent_bytes += x->bytes;
                s.extent_title_bytes += x->title_bytes;
                for (const Member& m : x->members) s.tombstones += !m.live;
            }
        s.split = split_, s.late = late_, s.no_room = no_room_, s.no_room_bytes = no_room_bytes_;
        s.refused = refused_, s.refused_bytes = refused_bytes_, s.moved = moved_;
        return s;
    }

private:
    static std::uint64_t align(std::uint64_t v, std::uint64_t a) { return a > 1 ? (v + a - 1) / a * a : v; }

    // The lowest offset where `c` overlaps only members it aliases (live or not), below `limit`; ~0 when none.
    static std::uint64_t fit(const std::vector<Member>& members, const Candidate& c, std::uint64_t limit) {
        std::vector<const Member*> conflicts;
        for (const Member& m : members)
            if (!aliases(m.runs, c.runs)) conflicts.push_back(&m);
        std::vector<std::uint64_t> starts{0};
        for (const Member* m : conflicts) starts.push_back(align(m->hi, c.alignment));
        std::sort(starts.begin(), starts.end());
        for (const std::uint64_t at : starts) {
            if (at + c.size > limit) break;
            const bool clear = std::none_of(conflicts.begin(), conflicts.end(), [&](const Member* m) {
                return m->lo < at + c.size && at < m->hi;
            });
            if (clear) return at;
        }
        return ~0ull;
    }

    std::shared_ptr<Extent> late_fit(const void* owner, Candidate& c, std::uint64_t& offset) {
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
        if (related.empty()) return {};
        std::sort(related.begin(), related.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (auto& [shared, x] : related) {
            const std::uint64_t at = fit(x->members, c, x->bytes);
            if (at == ~0ull) continue;
            x->members.push_back({owner, c.runs, at, at + c.size, true});
            c.extent = x;
            c.offset = offset = at;
            c.reserved = true;
            ++late_;
            return x;
        }
        ++no_room_, no_room_bytes_ += c.size;
        return {};
    }

    template <class Allocate>
    std::shared_ptr<Extent> make_extent(const void* seed_owner, Candidate& seed, Allocate& allocate) {
        std::vector<std::pair<const void*, Candidate*>> group{{seed_owner, &seed}};
        std::unordered_map<const Candidate*, bool> seen{{&seed, true}};
        for (std::size_t next = 0; next < group.size(); ++next) {
            const Candidate* g = group[next].second;
            for (auto& [owner, c] : candidates_) {
                if (c.reserved || c.placed || !c.mapped || seen.count(&c) || !aliases(c.runs, g->runs)) continue;
                seen[&c] = true;
                if (related_to_extent(c.runs)) continue;
                group.emplace_back(owner, &c);
            }
        }
        std::stable_sort(group.begin(), group.end(),
                         [](const auto& a, const auto& b) { return a.second->size > b.second->size; });
        auto x = std::make_shared<Extent>();
        std::uint64_t alignment = 1, own = 0;
        Runs all;
        for (const auto& [owner, c] : group) {
            const std::uint64_t at = fit(x->members, *c, ~0ull >> 1);
            x->members.push_back({owner, c->runs, at, at + c->size, true});
            c->offset = at;
            x->bytes = std::max(x->bytes, at + c->size);
            alignment = std::max(alignment, c->alignment);
            own += c->size;
            all.insert(all.end(), c->runs.begin(), c->runs.end());
        }
        x->bytes = align(x->bytes, std::max<std::uint64_t>(alignment, 65536));
        x->title_bytes = distinct(all);
        x->heap = allocate(x->bytes);
        if (!x->heap) {
            for (const auto& [owner, c] : group) c->offset = 0;
            return {};
        }
        metal_ledger::add(metal_ledger::render_target, x->bytes);
        for (const auto& [owner, c] : group) {
            c->extent = x;
            c->reserved = true;
        }
        extents_.push_back(x);
        if ((group.size() > 1 || x->bytes >= (4u << 20)) && logged_++ < 40)
            std::fprintf(stderr,
                         "[rt] rt-alias: extent of %zu render targets (%.2f MiB of textures) over %.2f MiB of title "
                         "memory: %.2f MiB\n",
                         group.size(), double(own) / 1048576.0, double(x->title_bytes) / 1048576.0,
                         double(x->bytes) / 1048576.0);
        return x;
    }

    // What the census would hold if every live candidate were laid out now, group by group.
    static void estimate(const std::vector<const Candidate*>& live, Stats& s) {
        std::vector<int> group(live.size(), -1);
        int groups = 0;
        for (std::size_t i = 0; i < live.size(); ++i) {
            if (group[i] >= 0) continue;
            std::vector<std::size_t> todo{i};
            group[i] = groups;
            std::vector<const Candidate*> members;
            while (!todo.empty()) {
                const std::size_t k = todo.back();
                todo.pop_back();
                members.push_back(live[k]);
                for (std::size_t j = 0; j < live.size(); ++j)
                    if (group[j] < 0 && aliases(live[k]->runs, live[j]->runs)) group[j] = groups, todo.push_back(j);
            }
            ++groups;
            std::sort(members.begin(), members.end(), [](const Candidate* a, const Candidate* b) { return a->size > b->size; });
            std::vector<Member> laid;
            std::uint64_t end = 0;
            for (const Candidate* c : members) {
                const std::uint64_t at = fit(laid, *c, ~0ull >> 1);
                laid.push_back({c, c->runs, at, at + c->size, true});
                end = std::max(end, at + c->size);
            }
            s.estimate_bytes += end;
        }
        s.groups = std::uint64_t(groups);
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
        w.valid = w.ever = false;
        w.clobbered_by = nullptr;
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
            if (o->clobbered_by == &w) o->clobbered_by = &tombstone_watch();
        }
        w.overlaps.clear();
    }

    static const Watch& tombstone_watch() {
        struct Named : Watch {
            Named() { std::snprintf(shape, sizeof shape, "a destroyed alias"); }
        };
        static const Named w;
        return w;
    }

    std::mutex mutex_;
    std::unordered_map<const void*, Candidate> candidates_;
    std::vector<std::weak_ptr<Extent>> extents_;
    std::uint64_t split_ = 0, late_ = 0, no_room_ = 0, no_room_bytes_ = 0, refused_ = 0, refused_bytes_ = 0,
                  moved_ = 0, logged_ = 0;
};

}  // namespace leonida::metal_rt_alias
