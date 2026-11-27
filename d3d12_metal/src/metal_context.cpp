#include "metal_context.h"
#include "vulkan_gpu_labels.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace leonida {

MetalContext::MetalContext(gpu::Device& device) : device_(device) {}

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

}  // namespace leonida
