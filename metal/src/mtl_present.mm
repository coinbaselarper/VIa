#include "mtl_internal.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <chrono>

#include <gta6rt_hud/stats.h>

namespace gta6rt::metal {

// Frame events (console WaitFrameEventX): a display link on a thread of its own, or a timer at the display rate
// when no screen gives one.
class FramePacer {
public:
    explicit FramePacer(double hz) : hz_(hz > 0 ? hz : 60.0) {}
    ~FramePacer() { stop(); }
    void start();
    void stop();
    void tick() {
        {
            std::lock_guard lock(mutex_);
            ++ticks_;
        }
        cv_.notify_all();
    }
    std::uint64_t ticks() const {
        std::lock_guard lock(mutex_);
        return ticks_;
    }
    bool wait_next(std::uint64_t timeout_ns) {
        std::unique_lock lock(mutex_);
        const std::uint64_t start = ticks_;
        return cv_.wait_for(lock, std::chrono::nanoseconds(timeout_ns), [&] { return ticks_ != start; });
    }
    bool display_link() const { return display_link_; }

private:
    double hz_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::uint64_t ticks_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    bool display_link_ = false;
    CFRunLoopRef loop_ = nullptr;
};

}  // namespace gta6rt::metal

@interface GTA6RTLinkTarget : NSObject
@property(nonatomic) gta6rt::metal::FramePacer* pacer;
- (void)step:(CADisplayLink*)link;
@end

@implementation GTA6RTLinkTarget
- (void)step:(CADisplayLink*)link {
    if (self.pacer) self.pacer->tick();
}
@end

namespace gta6rt::metal {

void FramePacer::start() {
    std::mutex m;
    std::condition_variable started;
    bool ready = false;
    thread_ = std::thread([&, this] {
        @autoreleasepool {
            CADisplayLink* link = nil;
            GTA6RTLinkTarget* target = [GTA6RTLinkTarget new];
            target.pacer = this;
            if (@available(macOS 14.0, *)) {
                NSScreen* screen = NSScreen.mainScreen;
                if (screen) link = [screen displayLinkWithTarget:target selector:@selector(step:)];
            }
            {
                std::lock_guard l(m);
                display_link_ = link != nil;
                loop_ = CFRunLoopGetCurrent();
                ready = true;
            }
            started.notify_all();
            if (link) {
                [link addToRunLoop:NSRunLoop.currentRunLoop forMode:NSRunLoopCommonModes];
                while (!stop_.load()) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
                [link invalidate];
            } else {
                const auto period = std::chrono::duration<double>(1.0 / hz_);
                auto next = std::chrono::steady_clock::now();
                while (!stop_.load()) {
                    next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
                    std::this_thread::sleep_until(next);
                    tick();
                }
            }
            target.pacer = nullptr;
        }
    });
    std::unique_lock l(m);
    started.wait(l, [&] { return ready; });
}

void FramePacer::stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    if (loop_) CFRunLoopWakeUp(loop_);
    thread_.join();
}

MetalSwapchain::MetalSwapchain(MetalDevice* device, const SwapchainDesc& desc, CAMetalLayer* layer,
                               std::vector<std::unique_ptr<MetalTexture>> buffers)
    : device_(device), desc_(desc), layer_(layer), buffers_(std::move(buffers)) {
    if (desc_.frame_events) {
        pacer_ = std::make_shared<FramePacer>(device_->caps().display_refresh_hz);
        pacer_->start();
        log("frame events: %s", pacer_->display_link() ? "CADisplayLink" : "timer at the display rate");
    }
}

MetalSwapchain::~MetalSwapchain() {
    alive_->store(false);
    if (pacer_) pacer_->stop();
    if (last_cb_) [last_cb_ waitUntilCompleted];
}

void MetalSwapchain::on_presented(double t) {
    std::lock_guard lock(stats_mutex_);
    if (t <= 0) return;
    if (last_presented_time_ > 0) {
        intervals_.push_back(t - last_presented_time_);
        if (intervals_.size() > 128) intervals_.pop_front();
    }
    last_presented_time_ = t;
    ++displayed_;
}

FrameStats MetalSwapchain::frame_stats() const {
    FrameStats s;
    std::lock_guard lock(stats_mutex_);
    s.presented = presented_;
    s.displayed = displayed_;
    s.last_presented_time = last_presented_time_;
    s.recent_intervals.assign(intervals_.begin(), intervals_.end());
    s.frame_events = pacer_ ? pacer_->ticks() : 0;
    s.refresh_hz = device_->caps().display_refresh_hz;
    return s;
}

Status MetalSwapchain::wait_frame_event(std::uint64_t timeout_ns) {
    if (!pacer_) return Status::not_ready;
    return pacer_->wait_next(timeout_ns) ? Status::ok : Status::timeout;
}

Status MetalSwapchain::encode_present(MetalQueue* queue, const PresentDesc& desc, bool advance) {
    @autoreleasepool {
        layer_.displaySyncEnabled = desc.sync_interval > 0;
        id<CAMetalDrawable> drawable = [layer_ nextDrawable];
        if (!drawable) return Status::timeout;
        id<MTLTexture> target = drawable.texture;
        const double W = double(target.width), H = double(target.height);
        id<MTLCommandBuffer> cb = queue->begin_command_buffer();
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = target;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        const EncoderFences f = queue->open_encoder(MetalQueue::Join::observe, {});
        const MTLRenderStages stages = MTLRenderStageVertex | MTLRenderStageFragment;
        for (std::uint32_t i = 0; i < f.wait_count; ++i) [enc waitForFence:f.wait[i] beforeStages:stages];
        NSMutableArray* kept = [NSMutableArray new];
        for (std::uint32_t i = 0; i < desc.plane_count; ++i) {
            const PresentPlane& pl = desc.planes[i];
            auto* t = static_cast<MetalTexture*>(pl.source);
            const PlaneBlend blend = i == 0 ? PlaneBlend::opaque : pl.blend;
            id<MTLRenderPipelineState> pso = device_->compose_pipeline(target.pixelFormat, blend);
            if (!pso) {
                [enc endEncoding];
                return Status::pipeline_failed;
            }
            const double tw = t->desc().width, th = t->desc().height;
            const bool whole_src = !pl.src.left && !pl.src.top && !pl.src.right && !pl.src.bottom;
            const bool whole_dst = !pl.dst.left && !pl.dst.top && !pl.dst.right && !pl.dst.bottom;
            const double sl = whole_src ? 0 : pl.src.left, st = whole_src ? 0 : pl.src.top;
            const double sr = whole_src ? tw : pl.src.right, sb = whole_src ? th : pl.src.bottom;
            const double dl = whole_dst ? 0 : pl.dst.left, dt = whole_dst ? 0 : pl.dst.top;
            const double dr = whole_dst ? W : pl.dst.right, db = whole_dst ? H : pl.dst.bottom;
            const float params[8] = {float(2 * dl / W - 1), float(1 - 2 * dt / H), float(2 * dr / W - 1), float(1 - 2 * db / H),
                                     float(sl / tw), float(st / th), float(sr / tw), float(sb / th)};
            [enc setRenderPipelineState:pso];
            [enc setVertexBytes:params length:sizeof(params) atIndex:0];
            [enc setFragmentTexture:t->mtl() atIndex:0];
            [enc setFragmentSamplerState:device_->compose_sampler(pl.filter) atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            [kept addObject:t->mtl()];
        }
        [enc updateFence:f.update afterStages:stages];
        [enc endEncoding];
        device_->counters().render_encoders++;
        if (desc_.keep_last_frame) {
            last_w_ = std::uint32_t(target.width);
            last_h_ = std::uint32_t(target.height);
            const std::size_t bytes = std::size_t(last_w_) * last_h_ * 4;
            if (!last_frame_ || last_frame_.length < bytes)
                last_frame_ = [device_->mtl() newBufferWithLength:bytes options:MTLResourceStorageModeShared];
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromTexture:target
                             sourceSlice:0
                             sourceLevel:0
                            sourceOrigin:MTLOriginMake(0, 0, 0)
                              sourceSize:MTLSizeMake(last_w_, last_h_, 1)
                                toBuffer:last_frame_
                       destinationOffset:0
                  destinationBytesPerRow:std::size_t(last_w_) * 4
                destinationBytesPerImage:bytes];
            [blit endEncoding];
            device_->counters().blit_encoders++;
        }
        std::weak_ptr<std::atomic<bool>> alive = alive_;
        MetalSwapchain* self = this;
        const double hud_present_at = CACurrentMediaTime();
        [drawable addPresentedHandler:^(id<MTLDrawable> d) {
          if (d.presentedTime > hud_present_at && gta6rt_hud_active())
              gta6rt_hud_note_display_latency_ns(std::uint64_t((d.presentedTime - hud_present_at) * 1e9));
          auto a = alive.lock();
          if (a && a->load()) self->on_presented(d.presentedTime);
        }];
        const double hz = device_->caps().display_refresh_hz > 0 ? device_->caps().display_refresh_hz : 60.0;
        if (desc.sync_interval > 1)
            [cb presentDrawable:drawable afterMinimumDuration:double(desc.sync_interval) / hz];
        else
            [cb presentDrawable:drawable];
        [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
          [kept removeAllObjects];
        }];
        queue->end_command_buffer(cb);
        last_cb_ = cb;
        if (advance) index_ = (index_ + 1) % std::uint32_t(buffers_.size());
        {
            std::lock_guard lock(stats_mutex_);
            presented_++;
        }
        device_->counters().presents++;
        gta6rt_hud_note_present();
        if (gta6rt_hud_active()) {
            AtomicStats& counters = device_->counters();
            gta6rt_hud_publish_pipelines(counters.pipelines_compiled.load(), counters.archive_hits.load(), 0);
        }
        device_->counters().present_planes += desc.plane_count;
    }
    return Status::ok;
}

Status MetalSwapchain::read_last_frame(std::vector<std::uint8_t>& bgra, std::uint32_t& width, std::uint32_t& height) {
    if (!desc_.keep_last_frame || !last_cb_ || !last_frame_) return Status::not_ready;
    [last_cb_ waitUntilCompleted];
    if (last_cb_.status != MTLCommandBufferStatusCompleted) return Status::device_lost;
    width = last_w_;
    height = last_h_;
    const std::size_t bytes = std::size_t(width) * height * 4;
    bgra.assign(static_cast<const std::uint8_t*>(last_frame_.contents),
                static_cast<const std::uint8_t*>(last_frame_.contents) + bytes);
    return Status::ok;
}

}  // namespace gta6rt::metal
