// The upscale stage in the Metal backend's command stream. At the title's ffxDispatch point the replay records
//   barrier (all) ; write_value(gate[slot], n) ; wait_value(done[slot] >= n)
// The backend commits everything before the wait and gates the rest on a CPU-released event (port/metal DESIGN 5.2).
// The stage's thread sees gate[slot] == n (every earlier encoder of that queue has finished: the write is a blit after
// the barrier's join), encodes the upscale into a command buffer of its own queue, and stores done[slot] = n from that
// command buffer's completed handler (success or failure), which releases the rest of the title's list. No GPU work of
// the title's runs concurrently with the upscale, and no GPU wait depends on the GPU (the CPU releases both sides).

#import <Metal/Metal.h>

#include "metal_upscaler.h"
#include "stage.h"

#include "../../../metal/src/mtl_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <string>
#include <thread>
#include <vector>

namespace gta6rt::upscale {

namespace {

using clock_type = std::chrono::steady_clock;
constexpr std::uint32_t kSlots = 512;

double ms_since(clock_type::time_point t) {
    return std::chrono::duration<double, std::milli>(clock_type::now() - t).count();
}

double env_ms(const char* name, double def) {
    const char* v = std::getenv(name);
    return v && *v ? std::strtod(v, nullptr) : def;
}

struct Job {
    std::uint64_t n = 0;
    std::uint32_t slot = 0;
    Frame frame;
    id<MTLTexture> color = nil, depth = nil, motion = nil, output = nil;
    clock_type::time_point queued;
};

struct Flight {
    std::uint64_t n;
    clock_type::time_point committed;
    bool warned = false;
};

class Stage {
public:
    static Stage* get() {
        static std::once_flag flag;
        static Stage* stage = nullptr;
        std::call_once(flag, [] {
            gpu::Device* dev = stage_device();
            if (!dev) return;
            auto* s = new Stage(*dev);
            if (s->ok_) stage = s;
        });
        return stage;
    }

    bool record(gpu::CommandList& out, Job&& job) {
        job.n = next_.fetch_add(1) + 1;
        job.slot = std::uint32_t(job.n % kSlots);
        job.queued = clock_type::now();
        gpu::Barrier b;
        b.type = gpu::BarrierType::uav;
        out.barrier({&b, 1});
        out.write_value(gate_.get(), std::uint64_t(job.slot) * 8, job.n, 8);
        out.wait_value(done_.get(), std::uint64_t(job.slot) * 8, job.n, 8, gpu::WaitCompare::greater_equal);
        {
            std::lock_guard lock(mutex_);
            pending_.push_back(std::move(job));
        }
        cv_.notify_one();
        return true;
    }

private:
    explicit Stage(gpu::Device& dev) : dev_(dev) {
        gpu::BufferDesc d;
        d.size = kSlots * 8;
        d.memory = gpu::MemoryKind::shared;
        d.usage = gpu::usage::copy;
        d.label = "gta6rt upscale gate";
        gate_ = dev.create_buffer(d);
        d.label = "gta6rt upscale done";
        done_ = dev.create_buffer(d);
        if (!gate_ || !done_ || !gate_->cpu_pointer() || !done_->cpu_pointer()) {
            std::fprintf(stderr, "[upscale] no shared gate buffers; the stage is off\n");
            return;
        }
        std::memset(gate_->cpu_pointer(), 0, kSlots * 8);
        std::memset(done_->cpu_pointer(), 0, kSlots * 8);
        gate_cpu_ = static_cast<volatile std::uint64_t*>(gate_->cpu_pointer());
        done_cpu_ = static_cast<std::uint64_t*>(done_->cpu_pointer());
        budget_ms_ = env_ms("GTA6RT_UPSCALE_BUDGET_MS", 8.0);
        if (const char* dump = std::getenv("GTA6RT_UPSCALE_DUMP"); dump && *dump) {
            std::string s = dump;
            const auto comma = s.find(',');
            dump_dir_ = s.substr(0, comma);
            if (comma != std::string::npos) {
                char* end = nullptr;
                dump_at_ = std::strtoull(s.c_str() + comma + 1, &end, 10);
                if (end && *end == ',') dump_every_ = std::strtoull(end + 1, nullptr, 10);
            }
            if (!dump_at_) dump_at_ = 600;
        }
        ok_ = true;
        worker_ = std::thread([this] { run(); });
        worker_.detach();
        std::fprintf(stderr, "[upscale] stage ready: %s, time budget %.1f ms a frame%s%s\n", mode_name(selected_mode()),
                     budget_ms_, dump_dir_.empty() ? "" : ", dump into ", dump_dir_.c_str());
    }

    MetalUpscaler& upscaler(id<MTLDevice> device, Mode mode) {
        auto& slot = mode == Mode::bilinear ? bilinear_ : main_;
        if (!slot || slot->mode() != mode) slot = std::make_unique<MetalUpscaler>(device, mode);
        return *slot;
    }

    void release(const Job& j) {
        __atomic_store_n(&done_cpu_[j.slot], j.n, __ATOMIC_RELEASE);
    }

    void run() {
        pthread_setname_np("gta6rt-upscale");
        auto report_at = clock_type::now() + std::chrono::seconds(10);
        for (;;) {
            std::vector<Job> ready;
            {
                std::unique_lock lock(mutex_);
                if (pending_.empty()) cv_.wait_for(lock, std::chrono::milliseconds(200));
                for (auto it = pending_.begin(); it != pending_.end();) {
                    const std::uint64_t g = __atomic_load_n(const_cast<std::uint64_t*>(&gate_cpu_[it->slot]), __ATOMIC_ACQUIRE);
                    if (g >= it->n) {
                        ready.push_back(std::move(*it));
                        it = pending_.erase(it);
                    } else if (ms_since(it->queued) > 30000.0) {
                        std::fprintf(stderr, "[upscale] frame %llu (n %llu): the title's work before it never reached "
                                             "the gate in 30 s; released without upscaling\n",
                                     static_cast<unsigned long long>(it->frame.id), static_cast<unsigned long long>(it->n));
                        release(*it);
                        it = pending_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            for (Job& j : ready) execute(j);
            watch_flights();
            if (clock_type::now() >= report_at) {
                report();
                report_at = clock_type::now() + std::chrono::seconds(10);
            }
            bool idle;
            {
                std::lock_guard lock(mutex_);
                idle = pending_.empty();
            }
            if (ready.empty() && !idle) std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }

    void watch_flights() {
        std::lock_guard lock(flight_mutex_);
        for (Flight& f : flights_)
            if (!f.warned && ms_since(f.committed) > 2000.0) {
                f.warned = true;
                std::fprintf(stderr, "[upscale] CULPRIT: command buffer gta6rt-upscale n %llu (%s) has run %.0f ms on "
                                     "the GPU queue without completing\n",
                             static_cast<unsigned long long>(f.n), mode_name(current_), ms_since(f.committed));
            }
    }

    void execute(Job& j) {
        const double gate_ms = ms_since(j.queued);
        id<MTLDevice> device = j.color.device;
        if (!queue_) {
            queue_ = [device newCommandQueue];
            queue_.label = @"gta6rt upscale";
            current_ = selected_mode();
        }
        MetalInputs in;
        in.color = j.color;
        in.depth = j.depth;
        in.motion = j.motion;
        in.output = j.output;
        in.render_w = j.frame.render_w ? j.frame.render_w : std::uint32_t(j.color.width);
        in.render_h = j.frame.render_h ? j.frame.render_h : std::uint32_t(j.color.height);
        in.output_w = j.frame.upscale_w ? j.frame.upscale_w : std::uint32_t(j.output.width);
        in.output_h = j.frame.upscale_h ? j.frame.upscale_h : std::uint32_t(j.output.height);
        in.jitter_x = j.frame.jitter_x;
        in.jitter_y = j.frame.jitter_y;
        in.motion_scale_x = j.frame.motion_scale_x;
        in.motion_scale_y = j.frame.motion_scale_y;
        in.pre_exposure = j.frame.pre_exposure;
        in.reset = j.frame.reset || j.frame.context != last_context_ || j.frame.id != last_frame_ + 1;
        in.depth_inverted = j.frame.create_flags & 8u;
        in.auto_exposure = j.frame.create_flags & 32u;
        in.hdr = j.frame.create_flags & 1u;
        last_context_ = j.frame.context;
        last_frame_ = j.frame.id;
        id<MTLCommandBuffer> cb = [queue_ commandBuffer];
        cb.label = [NSString stringWithFormat:@"gta6rt-upscale n %llu frame %llu %s", (unsigned long long)j.n,
                                              (unsigned long long)j.frame.id, mode_name(current_)];
        std::string why;
        Mode used = current_;
        bool ok = upscaler(device, current_).encode(cb, in, why);
        if (!ok && current_ != Mode::bilinear) {
            std::fprintf(stderr, "[upscale] %s refused frame %llu: %s; falling back to bilinear for the rest of the run\n",
                         mode_name(current_), static_cast<unsigned long long>(j.frame.id), why.c_str());
            current_ = Mode::bilinear;
            used = current_;
            ok = upscaler(device, current_).encode(cb, in, why);
        }
        if (!ok) {
            static std::atomic<int> said{0};
            if (said.fetch_add(1) < 8)
                std::fprintf(stderr, "[upscale] frame %llu not upscaled: %s (output left as it was)\n",
                             static_cast<unsigned long long>(j.frame.id), why.c_str());
            counters().failed.fetch_add(1);
        }
        id<MTLTexture> dump_in = nil, dump_out = nil, dump_depth = nil, dump_motion = nil;
        const std::uint64_t ran = counters().ran.load() + 1;
        if (ok && !dump_dir_.empty() && (ran == dump_at_ || (dump_every_ && ran > dump_at_ && (ran - dump_at_) % dump_every_ == 0))) {
            dump_in = MetalUpscaler::encode_readback(cb, j.color, in.render_w, in.render_h);
            dump_out = MetalUpscaler::encode_readback(cb, j.output, in.output_w, in.output_h);
            if (j.depth) dump_depth = MetalUpscaler::encode_readback(cb, j.depth, in.render_w, in.render_h);
            if (j.motion) dump_motion = MetalUpscaler::encode_readback(cb, j.motion, in.render_w, in.render_h);
        }
        const std::uint64_t n = j.n;
        const std::uint32_t slot = j.slot;
        const std::uint64_t frame = j.frame.id;
        const double budget = budget_ms_;
        std::uint64_t* done = done_cpu_;
        const std::string dir = dump_dir_;
        const auto commit_time = clock_type::now();
        {
            std::lock_guard lock(flight_mutex_);
            flights_.push_back({n, commit_time});
        }
        [cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
            const double gpu = (b.GPUEndTime - b.GPUStartTime) * 1000.0;
            const double wall = ms_since(commit_time);
            if (b.status == MTLCommandBufferStatusError) {
                counters().failed.fetch_add(1);
                std::fprintf(stderr, "[upscale] CULPRIT: command buffer %s failed: %s\n", b.label.UTF8String,
                             b.error.localizedDescription.UTF8String);
            } else if (ok) {
                counters().ran.fetch_add(1);
            }
            __atomic_store_n(&done[slot], n, __ATOMIC_RELEASE);
            if (gpu > budget) {
                const std::uint64_t k = counters().over_budget.fetch_add(1) + 1;
                if (k <= 8 || !(k & (k - 1)))
                    std::fprintf(stderr, "[upscale] over budget: %s ran %.2f ms on the GPU (budget %.1f ms; %llu so far)\n",
                                 b.label.UTF8String, gpu, budget, static_cast<unsigned long long>(k));
            }
            {
                std::lock_guard lock(stats_mutex_);
                gpu_ms_.push_back(gpu);
                wall_ms_.push_back(wall);
                gate_ms_.push_back(gate_ms);
            }
            {
                std::lock_guard lock(flight_mutex_);
                flights_.erase(std::remove_if(flights_.begin(), flights_.end(), [n](const Flight& f) { return f.n == n; }),
                               flights_.end());
            }
            if (dump_in && dump_out) {
                char a[64], c[64];
                std::snprintf(a, sizeof a, "/upscale-%06llu-input.png", static_cast<unsigned long long>(frame));
                std::snprintf(c, sizeof c, "/upscale-%06llu-output.png", static_cast<unsigned long long>(frame));
                std::string s1, s2;
                const bool w1 = MetalUpscaler::write_png(dir + a, dump_in, &s1),
                           w2 = MetalUpscaler::write_png(dir + c, dump_out, &s2);
                std::fprintf(stderr, "[upscale] dumped frame %llu: %s%s (%s; %s), %s%s (%s; %s)\n",
                             static_cast<unsigned long long>(frame), dir.c_str(), a, w1 ? "ok" : "FAILED", s1.c_str(),
                             dir.c_str(), c, w2 ? "ok" : "FAILED", s2.c_str());
                std::string s3, s4;
                if (dump_depth) MetalUpscaler::write_png(dir + "/upscale-last-depth.png", dump_depth, &s3);
                if (dump_motion) MetalUpscaler::write_png(dir + "/upscale-last-motion.png", dump_motion, &s4);
                std::fprintf(stderr, "[upscale] frame %llu inputs: depth %s; motion %s\n",
                             static_cast<unsigned long long>(frame), s3.c_str(), s4.c_str());
            }
        }];
        [cb commit];
        const std::uint64_t k = counters().replayed.load();
        if (k <= 3) {
            std::fprintf(stderr, "[upscale] frame %llu: %s colour %lux%lu fmt %lu, depth fmt %lu, motion fmt %lu -> output "
                                 "%lux%lu fmt %lu usage %#lx (render %ux%u -> %ux%u), gate after %.2f ms\n",
                         static_cast<unsigned long long>(j.frame.id), mode_name(used), (unsigned long)j.color.width,
                         (unsigned long)j.color.height, (unsigned long)j.color.pixelFormat,
                         (unsigned long)(j.depth ? j.depth.pixelFormat : 0), (unsigned long)(j.motion ? j.motion.pixelFormat : 0),
                         (unsigned long)j.output.width, (unsigned long)j.output.height, (unsigned long)j.output.pixelFormat,
                         (unsigned long)j.output.usage, in.render_w, in.render_h, in.output_w, in.output_h, gate_ms);
        }
    }

    void report() {
        std::vector<double> g, w, q;
        {
            std::lock_guard lock(stats_mutex_);
            g.swap(gpu_ms_);
            w.swap(wall_ms_);
            q.swap(gate_ms_);
        }
        if (g.empty() && !counters().dispatches.load()) return;
        auto pct = [](std::vector<double>& v, double p) {
            if (v.empty()) return 0.0;
            std::sort(v.begin(), v.end());
            return v[std::min(v.size() - 1, std::size_t(p * double(v.size())))];
        };
        const std::uint64_t mem = (main_ ? main_->memory_bytes() : 0) + (bilinear_ ? bilinear_->memory_bytes() : 0);
        std::fprintf(stderr,
                     "[upscale] last 10 s: %zu frames by %s; GPU ms p50 %.3f p95 %.3f max %.3f; commit->done ms p50 %.3f "
                     "max %.3f; replay->gate ms p50 %.2f max %.2f | totals: dispatches %llu, ran %llu, failed %llu, over "
                     "budget %llu, refused %llu, queries %llu; scratch %llu KiB\n",
                     g.size(), mode_name(current_), pct(g, 0.5), pct(g, 0.95), g.empty() ? 0.0 : g.back(), pct(w, 0.5),
                     w.empty() ? 0.0 : w.back(), pct(q, 0.5), q.empty() ? 0.0 : q.back(),
                     static_cast<unsigned long long>(counters().dispatches.load()),
                     static_cast<unsigned long long>(counters().ran.load()),
                     static_cast<unsigned long long>(counters().failed.load()),
                     static_cast<unsigned long long>(counters().over_budget.load()),
                     static_cast<unsigned long long>(counters().refused.load()),
                     static_cast<unsigned long long>(counters().queries.load()), static_cast<unsigned long long>(mem >> 10));
    }

    gpu::Device& dev_;
    bool ok_ = false;
    std::unique_ptr<gpu::Buffer> gate_, done_;
    volatile std::uint64_t* gate_cpu_ = nullptr;
    std::uint64_t* done_cpu_ = nullptr;
    std::atomic<std::uint64_t> next_{0};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> pending_;
    std::thread worker_;
    id<MTLCommandQueue> queue_ = nil;
    std::unique_ptr<MetalUpscaler> main_, bilinear_;
    Mode current_ = Mode::metalfx_temporal;
    std::uint64_t last_context_ = 0, last_frame_ = 0;
    double budget_ms_ = 8.0;
    std::string dump_dir_;
    std::uint64_t dump_at_ = 0, dump_every_ = 0;
    std::mutex stats_mutex_, flight_mutex_;
    std::vector<double> gpu_ms_, wall_ms_, gate_ms_;
    std::vector<Flight> flights_;
};

id<MTLTexture> native(gpu::Texture* t) {
    return t ? static_cast<gta6rt::metal::MetalTexture*>(t)->mtl() : nil;
}

}  // namespace

bool record_metal(std::string_view label, gpu::CommandList& out, const TextureOf& texture) {
    std::uint64_t id = 0;
    Frame f;
    if (!marker_frame(label, id) || !frame_for(id, f)) {
        static std::atomic<int> said{0};
        if (said.fetch_add(1) < 8)
            std::fprintf(stderr, "[upscale] marker %.*s: no such frame (older than 256 dispatches?); not upscaled\n",
                         int(label.size()), label.data());
        return false;
    }
    Stage* stage = Stage::get();
    if (!stage) return false;
    Job j;
    j.frame = f;
    j.color = native(texture(f.color));
    j.output = native(texture(f.output));
    j.depth = f.depth ? native(texture(f.depth)) : nil;
    j.motion = f.motion ? native(texture(f.motion)) : nil;
    if (!j.color || !j.output) {
        static std::atomic<int> said{0};
        if (said.fetch_add(1) < 8)
            std::fprintf(stderr, "[upscale] frame %llu: colour %#lx -> %s, output %#lx -> %s; not upscaled\n",
                         static_cast<unsigned long long>(f.id), static_cast<unsigned long>(f.color),
                         j.color ? "texture" : "NO TEXTURE", static_cast<unsigned long>(f.output),
                         j.output ? "texture" : "NO TEXTURE");
        return false;
    }
    counters().replayed.fetch_add(1);
    return stage->record(out, std::move(j));
}

}  // namespace gta6rt::upscale
