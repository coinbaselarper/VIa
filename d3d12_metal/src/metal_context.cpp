#include "metal_context.h"
#include "present_dump.h"
#include "vulkan_gpu_labels.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

namespace leonida {

MetalContext::MetalContext(gpu::Device& device) : device_(device) { scene_ = SceneProbe::create(*this); }

MetalContext::~MetalContext() = default;

void MetalContext::add_resource(ResourceId id, const MetalBinding& binding) {
    std::unique_lock lock(resource_mutex_);
    const auto old = resources_.find(id);
    if (old != resources_.end() && old->second.buffer && old->second.va) {
        auto [lo, hi] = vas_.equal_range(old->second.va);
        for (auto it = lo; it != hi; ++it)
            if (it->second.id == id) {
                vas_.erase(it);
                break;
            }
    }
    resources_[id] = binding;
    if (binding.buffer && binding.va && binding.size) {
        vas_.emplace(binding.va, VaRange{binding.va + binding.size, binding.buffer, binding.buffer_offset, id});
        max_range_ = std::max(max_range_, binding.size);
    }
}

void MetalContext::remove_resource(ResourceId id) {
    std::unique_lock lock(resource_mutex_);
    const auto it = resources_.find(id);
    if (it == resources_.end()) return;
    if (it->second.buffer && it->second.va) {
        auto [lo, hi] = vas_.equal_range(it->second.va);
        for (auto v = lo; v != hi; ++v)
            if (v->second.id == id) {
                vas_.erase(v);
                break;
            }
    }
    resources_.erase(it);
}

bool MetalContext::binding(ResourceId id, MetalBinding& out) const {
    std::shared_lock lock(resource_mutex_);
    const auto it = resources_.find(id);
    if (it == resources_.end()) return false;
    out = it->second;
    return true;
}

bool MetalContext::has_resource(ResourceId id) const {
    std::shared_lock lock(resource_mutex_);
    return resources_.count(id) != 0;
}

bool MetalContext::buffer_at(std::uint64_t va, std::uint64_t& bytes, gpu::Buffer*& buffer,
                             std::uint64_t& offset) const {
    std::shared_lock lock(resource_mutex_);
    auto it = vas_.upper_bound(va);
    const VaRange* best = nullptr;
    std::uint64_t best_start = 0;
    while (it != vas_.begin()) {
        --it;
        if (va - it->first > max_range_) break;
        if (va < it->second.end) {
            const bool holds_all = bytes <= it->second.end - va;
            const bool best_holds = best && bytes <= best->end - va;
            if (!best || (holds_all && !best_holds)) {
                best = &it->second;
                best_start = it->first;
                if (holds_all) break;
            }
        }
    }
    if (!best) {
        if (arena_buffer_ && va < arena_size_) {
            buffer = arena_buffer_;
            offset = va;
            bytes = std::min(bytes, arena_size_ - va);
            return true;
        }
        return false;
    }
    buffer = best->buffer;
    offset = best->offset + (va - best_start);
    bytes = std::min(bytes, best->end - va);
    return true;
}

bool MetalContext::host_at(std::uint64_t va, std::uint64_t bytes, const std::uint8_t*& host) const {
    std::shared_lock lock(resource_mutex_);
    auto it = vas_.upper_bound(va);
    while (it != vas_.begin()) {
        --it;
        if (va - it->first > max_range_) break;
        if (va < it->second.end && bytes <= it->second.end - va && it->second.id) {
            const auto r = resources_.find(it->second.id);
            if (r == resources_.end() || !r->second.host) return false;
            host = r->second.host + (va - it->first);
            return true;
        }
    }
    return false;
}

bool MetalContext::knows_buffer_va(std::uint64_t va, std::uint64_t bytes) const {
    std::shared_lock lock(resource_mutex_);
    auto it = vas_.upper_bound(va);
    while (it != vas_.begin()) {
        --it;
        if (va - it->first > max_range_) break;
        if (va < it->second.end && bytes <= it->second.end - va) return true;
    }
    return false;
}

bool MetalContext::buffer_resource_at(std::uint64_t va, std::uint64_t bytes, ResourceId& id, std::uint64_t& start,
                                      std::uint64_t& size, const std::function<bool(ResourceId)>& accept) const {
    std::shared_lock lock(resource_mutex_);
    auto it = vas_.upper_bound(va);
    while (it != vas_.begin()) {
        --it;
        if (va - it->first > max_range_) break;
        if (!it->second.id || va >= it->second.end || bytes > it->second.end - va) continue;
        if (accept && !accept(it->second.id)) continue;
        id = it->second.id;
        start = it->first;
        size = it->second.end - it->first;
        return true;
    }
    return false;
}

void MetalContext::add_va_range(std::uint64_t va, std::uint64_t bytes, gpu::Buffer* buffer, std::uint64_t offset) {
    if (!va || !bytes || !buffer) return;
    std::unique_lock lock(resource_mutex_);
    vas_.emplace(va, VaRange{va + bytes, buffer, offset, 0});
    max_range_ = std::max(max_range_, bytes);
}

void MetalContext::remove_va_range(std::uint64_t va, gpu::Buffer* buffer) {
    std::unique_lock lock(resource_mutex_);
    auto [lo, hi] = vas_.equal_range(va);
    for (auto it = lo; it != hi; ++it)
        if (it->second.buffer == buffer && !it->second.id) {
            vas_.erase(it);
            return;
        }
}

bool MetalContext::resolve_write(std::uint64_t va, gpu::Buffer*& buffer, std::uint64_t& offset) const {
    std::uint64_t bytes = 4;
    if (buffer_at(va, bytes, buffer, offset)) return true;
    return write_resolver_ && write_resolver_(va, buffer, offset);
}

void MetalContext::add_pipeline(std::uintptr_t id, const MetalComputePipeline& pipeline) {
    std::unique_lock lock(pipeline_mutex_);
    compute_[id] = pipeline;
}

void MetalContext::add_graphics_pipeline(std::uintptr_t id, const MetalGraphicsPipeline& pipeline) {
    std::unique_lock lock(pipeline_mutex_);
    graphics_[id] = pipeline;
}

void MetalContext::remove_pipeline(std::uintptr_t id) {
    std::unique_lock lock(pipeline_mutex_);
    compute_.erase(id);
    graphics_.erase(id);
}

bool MetalContext::compute_pipeline(std::uintptr_t id, MetalComputePipeline& out) const {
    std::shared_lock lock(pipeline_mutex_);
    const auto it = compute_.find(id);
    if (it == compute_.end()) return false;
    out = it->second;
    return true;
}

bool MetalContext::graphics_pipeline(std::uintptr_t id, MetalGraphicsPipeline& out) const {
    std::shared_lock lock(pipeline_mutex_);
    const auto it = graphics_.find(id);
    if (it == graphics_.end()) return false;
    out = it->second;
    return true;
}

void MetalContext::add_query_heap(QueryHeapId id, const QueryHeapEntry& entry) {
    std::unique_lock lock(pipeline_mutex_);
    queries_[id] = entry;
}

void MetalContext::remove_query_heap(QueryHeapId id) {
    std::unique_lock lock(pipeline_mutex_);
    queries_.erase(id);
}

bool MetalContext::query_heap(QueryHeapId id, QueryHeapEntry& out) const {
    std::shared_lock lock(pipeline_mutex_);
    const auto it = queries_.find(id);
    if (it == queries_.end()) return false;
    out = it->second;
    return true;
}

std::uint32_t MetalContext::query_heap_type(QueryHeapId id) const {
    QueryHeapEntry e;
    return query_heap(id, e) ? e.d3d12_type : UINT32_MAX;
}

void MetalContext::mark_device_lost(const char* where) {
    if (device_lost_.exchange(true)) return;
    std::fprintf(stderr, "[rt] d3d12-metal: device lost (%s)\n", where ? where : "?");
    if (on_lost_) on_lost_(where);
}

void MetalContext::note_unsupported(const char* what) {
    stats_.unsupported.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(unsupported_mutex_);
    const std::uint64_t n = ++unsupported_[what];
    if (n == 1 || n == 10 || n == 1000 || n == 100000)
        std::fprintf(stderr, "[rt] d3d12-metal: not implemented on Metal yet: %s (%llu)\n", what,
                     static_cast<unsigned long long>(n));
}

bool MetalContext::census_on() {
    static const bool on = [] {
        const char* v = std::getenv("GTA6RT_METAL_DRAW_CENSUS");
        return v && *v && *v != '0';
    }();
    return on;
}

void MetalContext::note_census(const char* pipeline, const char* what) {
    std::lock_guard lock(census_mutex_);
    ++census_[{pipeline ? pipeline : "?", what}];
}

std::string MetalContext::take_census() {
    std::map<std::pair<std::string, std::string>, std::uint64_t> taken;
    {
        std::lock_guard lock(census_mutex_);
        taken.swap(census_);
    }
    std::vector<std::pair<std::uint64_t, std::string>> rows;
    for (const auto& [k, n] : taken) rows.emplace_back(n, k.second + " " + k.first);
    std::sort(rows.rbegin(), rows.rend());
    std::string out;
    for (std::size_t i = 0; i < rows.size() && i < 24; ++i)
        out += (out.empty() ? "" : "; ") + rows[i].second + " x" + std::to_string(rows[i].first);
    return out;
}

const char* MetalContext::work_name(Work w) {
    static const char* names[] = {"draw", "draw indexed", "draw rect-list", "mesh", "mesh split64", "indirect draw",
                                  "indirect draw indexed", "indirect rect-list", "indirect mesh",
                                  "indirect mesh split64", "dispatch", "indirect dispatch",
                                  "ExecuteIndirect draw (no captured state)"};
    static_assert(sizeof(names) / sizeof(*names) == std::size_t(Work::count));
    return names[std::size_t(w)];
}

const char* MetalContext::fate_name(Fate f) {
    static const char* names[] = {"recorded", "PSO refused", "PSO unknown", "library pending", "pipeline compiling",
                                  "Metal refused the pipeline", "render target unknown", "vertex buffer unknown",
                                  "index buffer unknown", "arguments unknown", "not implemented",
                                  "a compute pass before it not ready"};
    static_assert(sizeof(names) / sizeof(*names) == std::size_t(Fate::count));
    return names[std::size_t(f)];
}

static std::int64_t guard_ns() {
    static const std::int64_t ns = [] {
        const char* v = std::getenv("GTA6RT_METAL_INDIRECT_GUARD_MS");
        return std::int64_t(v && *v ? std::strtoll(v, nullptr, 10) : 2000) * 1000000;
    }();
    return ns;
}

static std::int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void MetalContext::note_compute_not_ready() { last_compute_skip_ns_.store(steady_ns(), std::memory_order_relaxed); }

bool MetalContext::producers_settling() const {
    const std::int64_t g = guard_ns();
    const std::int64_t last = last_compute_skip_ns_.load(std::memory_order_relaxed);
    return g > 0 && last && steady_ns() - last < g;
}

void MetalContext::note_work(Work w, Fate f, const char* why) {
    work_[std::size_t(w) * std::size_t(Fate::count) + std::size_t(f)].fetch_add(1, std::memory_order_relaxed);
    if (f == Fate::recorded) return;
    std::lock_guard lock(work_mutex_);
    std::string key = std::string(work_name(w)) + ": " + fate_name(f);
    if (why && *why) key += std::string(" (") + std::string(why).substr(0, 140) + ")";
    ++work_why_[key];
}

std::string MetalContext::take_work_report(std::uint64_t presents) {
    const double frames = presents ? double(presents) : 1.0;
    std::string out;
    char text[160];
    for (std::size_t w = 0; w < std::size_t(Work::count); ++w) {
        std::string row;
        std::uint64_t total = 0;
        for (std::size_t f = 0; f < std::size_t(Fate::count); ++f) {
            const std::size_t i = w * std::size_t(Fate::count) + f;
            const std::uint64_t now = work_[i].load(std::memory_order_relaxed);
            const std::uint64_t n = now - work_seen_[i];
            work_seen_[i] = now;
            if (!n) continue;
            total += n;
            std::snprintf(text, sizeof text, "%s%s %.1f", row.empty() ? "" : ", ", fate_name(Fate(f)), double(n) / frames);
            row += text;
        }
        if (!total) continue;
        std::snprintf(text, sizeof text, "%s%s %.1f (", out.empty() ? "" : "; ", work_name(Work(w)), double(total) / frames);
        out += text + row + ")";
    }
    std::map<std::string, std::uint64_t> why;
    {
        std::lock_guard lock(work_mutex_);
        why.swap(work_why_);
    }
    std::vector<std::pair<std::uint64_t, std::string>> rows;
    for (const auto& [k, n] : why) rows.emplace_back(n, k);
    std::sort(rows.rbegin(), rows.rend());
    for (std::size_t i = 0; i < rows.size() && i < 12; ++i) {
        std::snprintf(text, sizeof text, "%s%.1f/frame ", i ? "; " : " || skipped: ", double(rows[i].first) / frames);
        out += text + rows[i].second;
    }
    return out;
}

void MetalContext::note_submitted(gpu::Fence* progress, std::uint64_t value) {
    std::lock_guard lock(submitted_mutex_);
    auto& v = submitted_[progress];
    v = std::max(v, value);
}

void MetalContext::forget_queue(gpu::Fence* progress) {
    std::lock_guard lock(submitted_mutex_);
    submitted_.erase(progress);
}

std::vector<std::pair<gpu::Fence*, std::uint64_t>> MetalContext::submitted() const {
    std::lock_guard lock(submitted_mutex_);
    return {submitted_.begin(), submitted_.end()};
}

bool MetalContext::reached(const std::vector<std::pair<gpu::Fence*, std::uint64_t>>& marks) const {
    std::lock_guard lock(submitted_mutex_);
    for (const auto& [fence, value] : marks)
        if (submitted_.count(fence) && fence->completed_value() < value) return false;
    return true;
}

// ---- MetalQueue ----

MetalQueue::MetalQueue(MetalContext& context, gpu::QueueType type, const char* label)
    : context_(context), type_(type) {
    queue_ = context.gpu().create_queue(type, label);
    progress_ = context.gpu().create_fence(0);
    if (!queue_ || !progress_) {
        queue_.reset();
        return;
    }
    context_.holds().attach(this);
}

MetalQueue::~MetalQueue() {
    if (!queue_) return;
    context_.forget_queue(progress_.get());
    context_.holds().detach(this);
    {
        std::lock_guard state(state_mutex_);
        if (!held_.empty()) {
            std::fprintf(stderr, "[rt] d3d12-metal: queue destroyed with %zu held operation(s) never submitted\n",
                         held_.size());
            context_.holds().held_changed(-static_cast<std::int64_t>(held_.size()));
            held_.clear();
        }
    }
    context_.holds().pump_ready();
    queue_->wait_idle(5'000'000'000ull);
}

std::uint64_t MetalQueue::completed() const { return progress_ ? progress_->completed_value() : 0; }

bool MetalQueue::idle(std::uint64_t timeout_ns) {
    const gpu::Status s = queue_->wait_idle(timeout_ns);
    if (s == gpu::Status::device_lost) context_.mark_device_lost("wait_idle");
    return s == gpu::Status::ok;
}

bool MetalQueue::waits_pending(const std::vector<gpu::Fence*>& fences, const std::vector<std::uint64_t>& values) const {
    for (std::size_t i = 0; i < fences.size(); ++i)
        if (context_.holds().pending(fences[i], values[i])) return true;
    return false;
}

bool MetalQueue::must_hold() const { return !held_.empty() || waits_pending(waits_, wait_values_); }

void MetalQueue::retire_pins(std::vector<std::shared_ptr<void>>& pins, std::uint64_t ticket) {
    for (auto& pin : pins) {
        if (ticket) pin_queue_.push_back({ticket, std::move(pin)});
        else retired_pins_.push_back(std::move(pin));
    }
    pins.clear();
}

void MetalQueue::drain_pins(std::vector<std::shared_ptr<void>>& out) {
    out.swap(retired_pins_);
    if (pin_queue_.empty()) return;
    const std::uint64_t done = completed();
    while (!pin_queue_.empty() && pin_queue_.front().ticket <= done) {
        out.push_back(std::move(pin_queue_.front().pin));
        pin_queue_.pop_front();
    }
}

void MetalQueue::hold(Deferred&& op) {
    op.seq = context_.holds().next_seq();
    if (op.kind != Deferred::Kind::marker) {
        op.waits = std::move(waits_);
        op.wait_values = std::move(wait_values_);
        op.wait_pins = std::move(wait_pins_);
        waits_.clear();
        wait_values_.clear();
        wait_pins_.clear();
    }
    held_.push_back(std::move(op));
    context_.holds().held_changed(1);
    context_.stats().holds.fetch_add(1, std::memory_order_relaxed);
}

void MetalQueue::consume_waits() {
    for (std::size_t i = 0; i < waits_.size(); ++i) queue_->wait(waits_[i], wait_values_[i]);
    waits_.clear();
    wait_values_.clear();
}

std::uint64_t MetalQueue::mark_progress() {
    ++submitted_;
    if (queue_->signal(progress_.get(), submitted_) != gpu::Status::ok)
        context_.mark_device_lost("progress signal");
    context_.note_submitted(progress_.get(), submitted_);
    return submitted_;
}

bool MetalQueue::submit_signal(gpu::Fence* fence, std::uint64_t value) {
    if (!context_.holds().would_signal(fence, value)) return true;
    consume_waits();
    const gpu::Status s = queue_->signal(fence, value);
    if (s != gpu::Status::ok) return false;
    const std::uint64_t ticket = mark_progress();
    retire_pins(wait_pins_, ticket);
    context_.holds().signaled(fence, value);
    return true;
}

gpu::CommandList* MetalQueue::list_slot(std::size_t index) {
    while (lists_.size() <= index) lists_.push_back(context_.gpu().create_command_list(type_));
    gpu::CommandList* list = lists_[index].get();
    if (list) list->reset();
    return list;
}

ReplayResult MetalQueue::execute_now(std::span<const CommandList* const> lists) {
    using clock = std::chrono::steady_clock;
    auto& st = context_.stats();
    const auto t0 = clock::now();
    context_.run_before_execute();
    const auto t1 = clock::now();
    struct Replayed {
        std::int64_t start = submit_phases::now_ns();
        ~Replayed() {
            submit_phases::add(submit_phases::replay, std::uint64_t(submit_phases::now_ns() - start));
            submit_phases::submitted();
        }
    } replayed;
    if (!context_.bindless_prepare()) return ReplayResult::unsupported;
    std::vector<gpu::CommandList*> recorded;
    recorded.reserve(lists.size());
    std::vector<label::Write> labels;
    std::vector<std::unique_ptr<gpu::Buffer>> temps;
    for (std::size_t i = 0; i < lists.size(); ++i) {
        gpu::CommandList* out = list_slot(i);
        if (!out) return ReplayResult::unsupported;
        const ReplayResult r = metal_replay(*lists[i], *out, context_, type_, &labels, &temps);
        if (r != ReplayResult::success) return r;
        if (const gpu::Status s = out->close(); s != gpu::Status::ok) {
            std::fprintf(stderr, "[rt] d3d12-metal: a recorded list did not close: %s\n", gpu::status_name(s));
            return ReplayResult::unsupported;
        }
        recorded.push_back(out);
    }
    const auto t2 = clock::now();
    consume_waits();
    const gpu::Status s = queue_->execute(recorded);
    if (s == gpu::Status::device_lost) context_.mark_device_lost("execute");
    if (s != gpu::Status::ok) {
        static std::atomic<std::uint64_t> failed{0};
        const std::uint64_t n = ++failed;
        if (n <= 4 || !(n & (n - 1)))
            std::fprintf(stderr, "[rt] d3d12-metal: execute: %s (%llu so far; the backend's [metal] line says why)\n",
                         gpu::status_name(s), static_cast<unsigned long long>(n));
        return ReplayResult::unsupported;
    }
    const std::uint64_t ticket = mark_progress();
    retire_pins(wait_pins_, ticket);
    if (!labels.empty()) {
        const gpu::ClockCalibration cal = queue_->clock_calibration();
        progress_->on_completion(ticket, [labels = std::move(labels), cal] {
            for (const label::Write& w : labels) {
                std::uint64_t value = w.value;
                if (w.clock == 1) {
                    const auto now = std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                       std::chrono::steady_clock::now().time_since_epoch())
                                                       .count());
                    value = cal.gpu_ticks + (now > cal.cpu_ns ? now - cal.cpu_ns : 0);
                }
                label::store_value(w.target, value);
            }
        });
    }
    const auto t3 = clock::now();
    const auto us = [](auto a, auto b) {
        return std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(b - a).count());
    };
    st.executes.fetch_add(1, std::memory_order_relaxed);
    st.lists.fetch_add(lists.size(), std::memory_order_relaxed);
    st.before_us.fetch_add(us(t0, t1), std::memory_order_relaxed);
    st.replay_us.fetch_add(us(t1, t2), std::memory_order_relaxed);
    st.execute_us.fetch_add(us(t2, t3), std::memory_order_relaxed);
    return ReplayResult::success;
}

ReplayResult MetalQueue::execute(std::span<const CommandList* const> lists, const PinFn& pin) {
    ReplayResult result = ReplayResult::success;
    bool held = false;
    std::vector<std::shared_ptr<void>> retired;
    {
        std::lock_guard state(state_mutex_);
        if (context_.is_device_lost()) {
            result = ReplayResult::unsupported;
        } else if (!must_hold()) {
            result = execute_now(lists);
            if (result == ReplayResult::success && pin) {
                std::vector<std::shared_ptr<void>> pins{pin()};
                retire_pins(pins, submitted_);
            }
        } else {
            Deferred op;
            op.kind = Deferred::Kind::batch;
            op.lists.reserve(lists.size());
            for (const CommandList* list : lists) op.lists.push_back(*list);
            if (pin) op.pins.push_back(pin());
            hold(std::move(op));
            held = true;
        }
        drain_pins(retired);
    }
    retired.clear();
    if (held) context_.holds().pump_ready();
    return result;
}

bool MetalQueue::signal(gpu::Fence* fence, std::uint64_t value, const PinFn& pin) {
    bool ok = true;
    std::vector<std::shared_ptr<void>> retired;
    {
        std::lock_guard state(state_mutex_);
        if (must_hold()) {
            Deferred op;
            op.kind = Deferred::Kind::signal;
            op.fence = fence;
            op.value = value;
            if (pin) op.pins.push_back(pin());
            hold(std::move(op));
        } else {
            ok = submit_signal(fence, value);
        }
        drain_pins(retired);
    }
    retired.clear();
    context_.holds().pump_ready();
    return ok;
}

void MetalQueue::wait(gpu::Fence* fence, std::uint64_t value, const PinFn& pin) {
    std::vector<std::shared_ptr<void>> retired;
    std::lock_guard state(state_mutex_);
    waits_.push_back(fence);
    wait_values_.push_back(value);
    if (pin) wait_pins_.push_back(pin());
    drain_pins(retired);
}

void MetalQueue::marker(DebugMarker::Kind kind, const char*) {
    (void)kind;
}

void MetalQueue::run(Deferred& op) {
    auto later_waits = std::move(waits_);
    auto later_values = std::move(wait_values_);
    auto later_pins = std::move(wait_pins_);
    waits_ = std::move(op.waits);
    wait_values_ = std::move(op.wait_values);
    wait_pins_ = std::move(op.wait_pins);
    switch (op.kind) {
    case Deferred::Kind::batch: {
        std::vector<const CommandList*> lists;
        for (const CommandList& list : op.lists) lists.push_back(&list);
        const ReplayResult result = execute_now(lists);
        if (result != ReplayResult::success) {
            int none = static_cast<int>(ReplayResult::success);
            held_failure_.compare_exchange_strong(none, static_cast<int>(result));
            std::fprintf(stderr, "[rt] d3d12-metal: a held ExecuteCommandLists failed when released (%d)\n",
                         static_cast<int>(result));
        }
        break;
    }
    case Deferred::Kind::signal:
        if (!submit_signal(op.fence, op.value))
            std::fprintf(stderr, "[rt] d3d12-metal: a held Signal failed when it was released\n");
        break;
    case Deferred::Kind::marker:
        break;
    }
    retire_pins(op.pins, submitted_);
    waits_.insert(waits_.end(), later_waits.begin(), later_waits.end());
    wait_values_.insert(wait_values_.end(), later_values.begin(), later_values.end());
    for (auto& p : later_pins) wait_pins_.push_back(std::move(p));
}

bool MetalQueue::release_ready() {
    std::vector<std::shared_ptr<void>> retired;
    bool released = false;
    {
        std::lock_guard state(state_mutex_);
        while (!held_.empty() &&
               (context_.is_device_lost() || !waits_pending(held_.front().waits, held_.front().wait_values))) {
            Deferred op = std::move(held_.front());
            held_.pop_front();
            context_.holds().held_changed(-1);
            if (!context_.is_device_lost()) run(op);
            released = true;
        }
        drain_pins(retired);
    }
    return released;
}

void MetalQueue::discard_waiting_on(gpu::Fence* fence) {
    std::vector<Deferred> dropped;
    std::lock_guard state(state_mutex_);
    for (auto it = held_.begin(); it != held_.end();) {
        if (std::find(it->waits.begin(), it->waits.end(), fence) == it->waits.end() &&
            !(it->kind == Deferred::Kind::signal && it->fence == fence)) {
            ++it;
            continue;
        }
        std::fprintf(stderr, "[rt] d3d12-metal: a fence was released while a queue held work naming it; dropped\n");
        dropped.push_back(std::move(*it));
        it = held_.erase(it);
        context_.holds().held_changed(-1);
    }
}

std::uint64_t MetalQueue::oldest_held_seq() {
    std::lock_guard state(state_mutex_);
    return held_.empty() ? 0 : held_.front().seq;
}


// ---- SceneProbe ----

namespace {

float half_float(std::uint16_t h) {
    const int e = (h >> 10) & 31, m = h & 1023;
    const float s = (h & 0x8000) ? -1.0f : 1.0f;
    if (!e) return s * std::ldexp(float(m), -24);
    if (e == 31) return m ? NAN : s * INFINITY;
    return s * std::ldexp(float(1024 + m), e - 25);
}

float small_float(std::uint32_t bits, int mantissa) {
    const std::uint32_t m = bits & ((1u << mantissa) - 1), e = bits >> mantissa;
    if (!e) return std::ldexp(float(m), -14 - mantissa);
    if (e == 31) return m ? NAN : INFINITY;
    return std::ldexp(float((1u << mantissa) + m), int(e) - 15 - mantissa);
}

// DXGI texel to up to 4 floats (snorm as -1..1); the channel count, 0 for a format not decoded here.
int decode_texel(std::uint32_t f, const std::uint8_t* p, float v[4]) {
    std::uint32_t w;
    std::memcpy(&w, p, 4);
    switch (f) {
    case 2: case 6: std::memcpy(v, p, f == 2 ? 16 : 12); return f == 2 ? 4 : 3;
    case 10: case 34: case 54: {
        const int n = f == 10 ? 4 : f == 34 ? 2 : 1;
        for (int c = 0; c < n; ++c) {
            std::uint16_t h;
            std::memcpy(&h, p + 2 * c, 2);
            v[c] = half_float(h);
        }
        return n;
    }
    case 11: case 35: case 56: {
        const int n = f == 11 ? 4 : f == 35 ? 2 : 1;
        for (int c = 0; c < n; ++c) {
            std::uint16_t h;
            std::memcpy(&h, p + 2 * c, 2);
            v[c] = float(h) / 65535.0f;
        }
        return n;
    }
    case 13: case 37: {
        const int n = f == 13 ? 4 : 2;
        for (int c = 0; c < n; ++c) {
            std::int16_t h;
            std::memcpy(&h, p + 2 * c, 2);
            v[c] = std::max(-1.0f, float(h) / 32767.0f);
        }
        return n;
    }
    case 16: std::memcpy(v, p, 8); return 2;
    case 20: case 39: case 40: case 41: case 45: std::memcpy(v, p, 4); return 1;
    case 55: v[0] = float(w & 0xffff) / 65535.0f; return 1;
    case 24: v[0] = float(w & 1023) / 1023.0f, v[1] = float((w >> 10) & 1023) / 1023.0f;
        v[2] = float((w >> 20) & 1023) / 1023.0f, v[3] = float(w >> 30) / 3.0f; return 4;
    case 26: v[0] = small_float(w & 2047, 6), v[1] = small_float((w >> 11) & 2047, 6), v[2] = small_float(w >> 22, 5);
        return 3;
    case 28: case 29: case 30: case 87: case 91: case 88:
        for (int c = 0; c < 4; ++c) v[c] = float(p[c]) / 255.0f;
        if (f == 87 || f == 91 || f == 88) std::swap(v[0], v[2]);
        return 4;
    case 31: for (int c = 0; c < 4; ++c) v[c] = std::max(-1.0f, float(std::int8_t(p[c])) / 127.0f); return 4;
    case 49: v[0] = float(p[0]) / 255.0f, v[1] = float(p[1]) / 255.0f; return 2;
    case 51: v[0] = std::max(-1.0f, float(std::int8_t(p[0])) / 127.0f), v[1] = std::max(-1.0f, float(std::int8_t(p[1])) / 127.0f);
        return 2;
    case 61: case 62: case 65: v[0] = float(p[0]) / 255.0f; return 1;
    case 42: case 57: v[0] = float(f == 42 ? w : (w & 0xffff)); return 1;
    default: return 0;
    }
}

std::string short_name(const std::string& s) { return s.size() > 90 ? s.substr(0, 90) + "..." : s; }

std::string target_text(const SceneProbe::Target& t) {
    char b[64];
    std::snprintf(b, sizeof b, "f%u %ux%u #%llx", t.format, t.width, t.height, static_cast<unsigned long long>(t.id));
    return b;
}

}  // namespace

std::unique_ptr<SceneProbe> SceneProbe::create(MetalContext& context) {
    const char* p = std::getenv("GTA6RT_METAL_PRESENT_SCENE");
    const char* d = std::getenv("GTA6RT_METAL_SCENE_DUMP");
    const char* l = std::getenv("GTA6RT_METAL_SCENE_LOG");
    if (!(p && *p && *p != '0') && !(d && *d) && !(l && *l == '1')) return nullptr;
    return std::make_unique<SceneProbe>(context);
}

SceneProbe::SceneProbe(MetalContext& context) : context_(context) {
    if (const char* s = std::getenv("GTA6RT_METAL_SCENE_SIZE"); s && *s) {
        unsigned w = 0, h = 0;
        if (std::sscanf(s, "%ux%u", &w, &h) == 2 && w && h) width_ = w, height_ = h;
    }
    if (const char* p = std::getenv("GTA6RT_METAL_PRESENT_SCENE"); p && *p) present_mode_ = std::clamp(std::atoi(p), 0, 2);
    if (const char* e = std::getenv("GTA6RT_METAL_SCENE_EXPOSURE"); e && *e) fixed_exposure_ = std::strtof(e, nullptr);
    if (const char* d = std::getenv("GTA6RT_METAL_SCENE_DUMP"); d && *d) {
        std::string v = d;
        const std::size_t c = v.find(',');
        dump_dir_ = v.substr(0, c);
        if (c != std::string::npos) {
            unsigned long long a = 0, b = 0, n = 0;
            const int got = std::sscanf(v.c_str() + c + 1, "%llu,%llu,%llu", &a, &b, &n);
            if (got >= 1 && a) dump_first_ = a;
            if (got >= 2 && b) dump_every_ = b;
            if (got >= 3) dump_count_ = n;
        }
        ::mkdir(dump_dir_.c_str(), 0755);
    }
    if (const char* l = std::getenv("GTA6RT_METAL_SCENE_LOG"); l && *l == '1') log_only_ = dump_dir_.empty();
    std::fprintf(stderr,
                 "[rt] d3d12-metal scene probe: render size %ux%u, present %s, %s%s (frame %llu, then every %llu, %llu "
                 "times)\n",
                 width_, height_, present_mode_ == 1 ? "scene under the UI plane" : present_mode_ == 2 ? "scene alone" : "off",
                 dump_dir_.empty() ? (log_only_ ? "pass log" : "no dumps") : "dumps to ", dump_dir_.c_str(),
                 static_cast<unsigned long long>(dump_first_), static_cast<unsigned long long>(dump_every_),
                 static_cast<unsigned long long>(dump_count_));
}

SceneProbe::~SceneProbe() = default;

bool SceneProbe::hdr_format(std::uint32_t f) { return f == 2 || f == 10 || f == 26 || f == 67; }

void SceneProbe::draw(const Target* rts, std::uint32_t count, const Target& depth, const char* pipeline,
                      const char* kind, bool recorded) {
    std::lock_guard lock(mutex_);
    const char* name = pipeline ? pipeline : "";
    if (logging_ && passes_.size() < 600) {
        Pass* last = passes_.empty() ? nullptr : &passes_.back();
        const auto same = [&](const Pass& p) {
            if (p.dispatch || p.count != count || p.depth.id != depth.id) return false;
            for (std::uint32_t i = 0; i < count; ++i)
                if (p.rts[i].id != rts[i].id) return false;
            return true;
        };
        if (!last || !same(*last)) {
            Pass p;
            p.count = std::min<std::uint32_t>(count, 8);
            for (std::uint32_t i = 0; i < p.count; ++i) p.rts[i] = rts[i];
            p.depth = depth;
            p.first = name;
            p.kind = kind ? kind : "";
            passes_.push_back(std::move(p));
            last = &passes_.back();
        }
        last->last = name;
        ++last->calls;
        if (recorded) ++last->recorded;
        if (kind && last->kind.find(kind) == std::string::npos && last->kind.size() < 60)
            last->kind += std::string(last->kind.empty() ? "" : "+") + kind;
    }
    if (latched_) return;
    std::uint32_t at_size = 0;
    for (std::uint32_t i = 0; i < count; ++i)
        if (rts[i].id && render_size(rts[i])) ++at_size;
    if (count >= 3 && at_size >= 3) {
        if (count >= gbuffer_count_) {
            gbuffer_count_ = std::min<std::uint32_t>(count, 8);
            for (std::uint32_t i = 0; i < gbuffer_count_; ++i) gbuffer_[i] = rts[i];
            if (gbuffer_by_ != name) gbuffer_by_ = name;
            if (depth.id && render_size(depth)) gbuffer_depth_ = depth;
        }
        return;
    }
    for (std::uint32_t i = 0; i < count; ++i)
        if (rts[i].id && render_size(rts[i]) && hdr_format(rts[i].format)) {
            hdr_ = rts[i];
            if (hdr_by_ != name) hdr_by_ = name;
        }
    if (!gbuffer_depth_.id && depth.id && render_size(depth)) gbuffer_depth_ = depth;
}

bool SceneProbe::queue_readback(gpu::CommandList& out, gpu::Texture* texture, const Target& t, const std::string& name,
                                bool exposure_only) {
    gpu::FormatInfo info;
    if (!texture || (!t.depth && (!context_.gpu().describe_format(t.format, info) || !info.block_bytes ||
                                  info.block_dim != 1))) {
        std::fprintf(stderr, "[rt] d3d12-metal scene: %s (%s) not read back: format not described\n", name.c_str(),
                     target_text(t).c_str());
        return false;
    }
    Readback r;
    r.name = name;
    r.target = t;
    r.texel = t.depth ? (t.format == 55 ? 2 : 4) : info.block_bytes;
    r.pitch = t.width * r.texel;
    r.frame = frame_;
    r.exposure_only = exposure_only;
    gpu::BufferDesc d;
    d.size = std::uint64_t(r.pitch) * t.height;
    d.memory = gpu::MemoryKind::shared;
    d.usage = gpu::usage::copy;
    d.label = "scene readback";
    r.buffer = context_.gpu().create_buffer(d);
    if (!r.buffer || !r.buffer->cpu_pointer()) return false;
    gpu::BufferTextureCopy k;
    k.buffer = r.buffer.get();
    k.row_pitch = r.pitch;
    k.texture = texture;
    k.mip = t.mip;
    k.layer = t.layer;
    k.width = t.width;
    k.height = t.height;
    k.depth_plane = t.depth;
    out.copy_texture_to_buffer(k);
    if (!exposure_only && !dump_dir_.empty()) {
        char file[96];
        std::snprintf(file, sizeof file, "/scene_%05llu_%s.png", static_cast<unsigned long long>(upscaled_frames_), name.c_str());
        r.path = dump_dir_ + file;
    }
    readbacks_.push_back(std::move(r));
    return true;
}

bool SceneProbe::dispatch(const char* pipeline, bool recorded, gpu::CommandList& out, const Resolve& resolve) {
    std::lock_guard lock(mutex_);
    const std::string name = pipeline ? pipeline : "";
    if (logging_ && passes_.size() < 600) {
        if (passes_.empty() || !passes_.back().dispatch || passes_.back().first != name) {
            Pass p;
            p.dispatch = true;
            p.first = p.last = name;
            p.kind = "dispatch";
            passes_.push_back(std::move(p));
        }
        ++passes_.back().calls;
        if (recorded) ++passes_.back().recorded;
    }
    const bool upscaler = name.compare(0, 5, "fspx/") == 0 || name.find("GTA6RT_METAL_REFUSE_CS") != std::string::npos ||
                          name.compare(0, 14, "gta6rt-upscale") == 0;
    if (!upscaler || latched_) return false;
    latched_ = true;
    ++upscaled_frames_;
    if (upscaled_frames_ == 1 || upscaled_frames_ == 10 || upscaled_frames_ % 100 == 0)
        std::fprintf(stderr, "[rt] d3d12-metal scene latch %llu (frame %llu, at %s): colour %s by %s; G-buffer %u by %s\n",
                     static_cast<unsigned long long>(upscaled_frames_), static_cast<unsigned long long>(frame_),
                     short_name(name).c_str(), hdr_.id ? target_text(hdr_).c_str() : "none", short_name(hdr_by_).c_str(),
                     gbuffer_count_, short_name(gbuffer_by_).c_str());
    bool copied = false;
    gpu::Texture* colour = hdr_.id ? resolve(hdr_.id) : nullptr;
    if (present_mode_ && colour) {
        if (!copy_ || copy_->desc().format != hdr_.format || copy_->desc().width != hdr_.width ||
            copy_->desc().height != hdr_.height) {
            gpu::TextureDesc d;
            d.format = hdr_.format;
            d.width = hdr_.width;
            d.height = hdr_.height;
            d.usage = gpu::usage::shader_read | gpu::usage::copy;
            d.label = "scene colour (diagnostic present)";
            auto fresh = context_.gpu().create_texture(d);
            if (fresh) {
                static std::vector<std::unique_ptr<gpu::Texture>> kept;
                if (copy_) kept.push_back(std::move(copy_));
                copy_ = std::move(fresh);
            }
        }
        if (copy_) {
            gpu::TextureCopy k;
            k.src = colour;
            k.src_mip = hdr_.mip;
            k.src_layer = hdr_.layer;
            k.dst = copy_.get();
            k.width = hdr_.width;
            k.height = hdr_.height;
            out.copy_texture(k);
            copy_valid_ = true;
            copy_frame_ = frame_;
            copied = true;
        }
        const bool exposure_due = fixed_exposure_ <= 0 && (upscaled_frames_ % 30) == 1 &&
                                  std::none_of(readbacks_.begin(), readbacks_.end(), [](const Readback& r) { return r.exposure_only; });
        if (exposure_due && queue_readback(out, colour, hdr_, "exposure", true)) copied = true;
    }
    const std::uint64_t due = dump_first_ + dumps_done_ * dump_every_;
    if ((!dump_dir_.empty() || log_only_) && dumps_done_ < dump_count_ && upscaled_frames_ >= due) {
        ++dumps_done_;
        std::fprintf(stderr,
                     "[rt] d3d12-metal scene frame %llu (upscaled frame %llu): colour %s by %s; G-buffer %u targets by %s; "
                     "depth %s\n",
                     static_cast<unsigned long long>(frame_), static_cast<unsigned long long>(upscaled_frames_),
                     hdr_.id ? target_text(hdr_).c_str() : "none", short_name(hdr_by_).c_str(), gbuffer_count_,
                     short_name(gbuffer_by_).c_str(), gbuffer_depth_.id ? target_text(gbuffer_depth_).c_str() : "none");
        if (!dump_dir_.empty()) {
            if (colour) copied |= queue_readback(out, colour, hdr_, "colour", false);
            for (std::uint32_t i = 0; i < gbuffer_count_; ++i)
                if (gpu::Texture* t = gbuffer_[i].id ? resolve(gbuffer_[i].id) : nullptr)
                    copied |= queue_readback(out, t, gbuffer_[i], "gbuffer" + std::to_string(i), false);
            if (gpu::Texture* t = gbuffer_depth_.id ? resolve(gbuffer_depth_.id) : nullptr)
                copied |= queue_readback(out, t, gbuffer_depth_, "depth", false);
        }
    }
    return copied;
}

void SceneProbe::write_readback(Readback& r) {
    const Target& t = r.target;
    const auto* src = static_cast<const std::uint8_t*>(r.buffer->cpu_pointer());
    const std::size_t n = std::size_t(t.width) * t.height;
    std::vector<float> px(n * 4, 0.0f);
    int channels = 0;
    std::uint64_t bad = 0;
    for (std::uint32_t y = 0; y < t.height; ++y)
        for (std::uint32_t x = 0; x < t.width; ++x) {
            float v[4] = {0, 0, 0, 0};
            const int c = decode_texel(t.format, src + std::size_t(y) * r.pitch + std::size_t(x) * r.texel, v);
            channels = std::max(channels, c);
            for (int i = 0; i < 4; ++i) {
                if (!std::isfinite(v[i])) ++bad, v[i] = 0;
                px[(std::size_t(y) * t.width + x) * 4 + i] = v[i];
            }
        }
    if (!channels) {
        std::fprintf(stderr, "[rt] d3d12-metal scene: %s (%s): format %u not decoded\n", r.name.c_str(),
                     target_text(t).c_str(), t.format);
        return;
    }
    double lum_log = 0, lum_sum = 0, lum_max = 0;
    std::uint64_t zero = 0, lit = 0;
    std::array<double, 4> lo{1e30, 1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30, -1e30}, sum{};
    for (std::size_t i = 0; i < n; ++i) {
        const float* v = &px[i * 4];
        for (int c = 0; c < channels; ++c) lo[c] = std::min(lo[c], double(v[c])), hi[c] = std::max(hi[c], double(v[c])), sum[c] += v[c];
        const double l = channels >= 3 ? 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2] : v[0];
        if (l <= 0 && (channels < 2 || (v[1] <= 0 && (channels < 3 || v[2] <= 0)))) ++zero;
        else ++lit;
        lum_sum += std::max(0.0, l);
        lum_max = std::max(lum_max, l);
        lum_log += std::log(1e-4 + std::max(0.0, l));
    }
    const double log_avg = std::exp(lum_log / double(n));
    if (r.exposure_only) {
        if (lit) exposure_ = float(std::clamp(0.43 / std::max(log_avg, 1e-6), 1e-4, 1e4));
        static std::atomic<int> told{0};
        if (told.fetch_add(1) < 6)
            std::fprintf(stderr, "[rt] d3d12-metal scene exposure: log-average luminance %.4g, %.1f%% of texels lit -> %.4g\n",
                         log_avg, 100.0 * double(lit) / double(n), exposure_);
        return;
    }
    std::string stats;
    char b[160];
    for (int c = 0; c < channels; ++c) {
        std::snprintf(b, sizeof b, "%s%c %.4g..%.4g mean %.4g", c ? ", " : "", "rgba"[c], lo[c], hi[c], sum[c] / double(n));
        stats += b;
    }
    std::vector<std::uint8_t> rgba(n * 4);
    const bool hdr = hdr_format(t.format);
    const double exposure = hdr ? std::clamp(0.43 / std::max(log_avg, 1e-6), 1e-4, 1e4) : 1.0;
    const double depth_max = t.depth || channels == 1 ? std::max(hi[0], 1e-30) : 1.0;
    for (std::size_t i = 0; i < n; ++i) {
        const float* v = &px[i * 4];
        std::uint8_t* d = &rgba[i * 4];
        d[3] = 255;
        if (t.depth) {
            const double g = v[0] > 0 ? std::clamp(1.0 + std::log10(double(v[0]) / depth_max) / 4.0, 0.0, 1.0) : 0.0;
            d[0] = d[1] = d[2] = std::uint8_t(g * 255 + 0.5);
            continue;
        }
        for (int c = 0; c < 3; ++c) {
            double x = c < channels ? v[c] : (channels == 1 ? v[0] : 0.0);
            if (hdr) {
                x = 1.0 - std::exp(-std::max(0.0, x) * exposure);
                x = x <= 0.0031308 ? x * 12.92 : 1.055 * std::pow(x, 1.0 / 2.4) - 0.055;
            } else if (lo[c < channels ? c : 0] < 0) {
                x = x * 0.5 + 0.5;
            }
            d[c] = std::uint8_t(std::clamp(x, 0.0, 1.0) * 255 + 0.5);
        }
    }
    bool written = false;
    if (std::FILE* f = std::fopen(r.path.c_str(), "wb")) {
        written = present_dump::write_png(f, VkExtent2D{t.width, t.height}, VK_FORMAT_R8G8B8A8_UNORM, false, rgba.data());
        std::fclose(f);
    }
    std::fprintf(stderr,
                 "[rt] d3d12-metal scene %s (%s): %.1f%% of texels non-zero, %llu not finite; %s%s%s; %s %s\n",
                 r.name.c_str(), target_text(t).c_str(), 100.0 * double(lit) / double(n),
                 static_cast<unsigned long long>(bad), stats.c_str(),
                 hdr ? "; luminance log-average " : "", hdr ? (std::to_string(log_avg) + ", max " + std::to_string(lum_max)).c_str() : "",
                 written ? "written to" : "NOT written to", r.path.c_str());
}

void SceneProbe::finish_readbacks(bool all) {
    for (auto it = readbacks_.begin(); it != readbacks_.end();) {
        if (!all && frame_ < it->frame + 2) {
            ++it;
            continue;
        }
        write_readback(*it);
        it = readbacks_.erase(it);
    }
}

void SceneProbe::end_frame() {
    std::lock_guard lock(mutex_);
    if (logging_ && latched_ && !passes_.empty()) {
        std::fprintf(stderr, "[rt] d3d12-metal scene frame %llu (upscaled frame %llu): %zu passes\n",
                     static_cast<unsigned long long>(frame_), static_cast<unsigned long long>(upscaled_frames_),
                     passes_.size());
        for (std::size_t i = 0; i < passes_.size(); ++i) {
            const Pass& p = passes_[i];
            std::string targets;
            for (std::uint32_t k = 0; k < p.count; ++k)
                targets += (k ? " | " : "") + (p.rts[k].id ? target_text(p.rts[k]) : std::string("-"));
            if (p.depth.id) targets += (targets.empty() ? "D " : " ; D ") + target_text(p.depth);
            std::fprintf(stderr, "[rt] d3d12-metal scene pass %zu: %s x%llu (recorded %llu) [%s]: %s%s%s\n", i,
                         p.kind.c_str(), static_cast<unsigned long long>(p.calls),
                         static_cast<unsigned long long>(p.recorded), targets.c_str(), short_name(p.first).c_str(),
                         p.last != p.first ? " .. " : "", p.last != p.first ? short_name(p.last).c_str() : "");
        }
    }
    passes_.clear();
    ++frame_;
    finish_readbacks(false);
    const std::uint64_t due = dump_first_ + dumps_done_ * dump_every_;
    logging_ = (!dump_dir_.empty() || log_only_) && dumps_done_ < dump_count_ && upscaled_frames_ + 1 >= due;
    latched_ = false;
    hdr_ = {};
    hdr_by_.clear();
    gbuffer_count_ = 0;
    gbuffer_by_.clear();
    gbuffer_depth_ = {};
}

gpu::Texture* SceneProbe::present_source(float& exposure) {
    std::lock_guard lock(mutex_);
    if (!present_mode_ || !copy_valid_ || !copy_ || frame_ > copy_frame_ + 30) return nullptr;
    exposure = fixed_exposure_ > 0 ? fixed_exposure_ : exposure_;
    return copy_.get();
}

}  // namespace leonida
