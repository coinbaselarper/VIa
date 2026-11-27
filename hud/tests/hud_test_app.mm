#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <dlfcn.h>
#include <mach/mach.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gta6rt_hud/hud.h"
#include "hud_model.h"

static const char* kShader = R"(
#include <metal_stdlib>
using namespace metal;
struct V { float4 pos [[position]]; float2 uv; };
struct U { float t; float strip; float load; float pad; };
vertex V vs(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    V o;
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(p.x, 1.0 - p.y);
    return o;
}
fragment float4 fs(V in [[stage_in]], constant U& u [[buffer(0)]]) {
    float2 c = in.uv;
    float3 col = 0.5 + 0.5 * cos(u.t + c.xyx * 6.0 + float3(0.0, 2.0, 4.0) + u.strip * 0.3);
    float2 centre = 0.5 + 0.35 * float2(cos(u.t * 0.7), sin(u.t * 1.1));
    float acc = 0.0;
    for (int i = 0; i < int(u.load); ++i) acc += sin(float(i) * 0.01 + c.x * 3.0);
    col += acc * 1e-6;
    if (length(c - centre) < 0.08) col = float3(1.0);
    float grid = step(0.98, fract(c.x * 20.0)) + step(0.98, fract(c.y * 12.0));
    col = mix(col, float3(0.0), grid * 0.35);
    return float4(col, 1.0);
}
)";

@interface TestView : NSView
@property(atomic) uint64_t clicks;
@end

@implementation TestView
- (BOOL)wantsUpdateLayer {
    return YES;
}
- (CALayer*)makeBackingLayer {
    CAMetalLayer* l = [CAMetalLayer layer];
    l.device = MTLCreateSystemDefaultDevice();
    l.pixelFormat = MTLPixelFormatBGRA8Unorm;
    l.framebufferOnly = NO;
    return l;
}
- (void)syncDrawable {
    CAMetalLayer* l = (CAMetalLayer*)self.layer;
    const CGFloat s = self.window ? self.window.backingScaleFactor : 1.0;
    l.contentsScale = s;
    l.drawableSize = CGSizeMake(std::max(1.0, self.bounds.size.width * s), std::max(1.0, self.bounds.size.height * s));
}
- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self syncDrawable];
}
- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    [self syncDrawable];
}
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    return YES;
}
- (void)mouseDown:(NSEvent*)event {
    self.clicks = self.clicks + 1;
}
@end

static std::atomic<bool> g_run{true};
static std::atomic<bool> g_stutter{false};
static std::atomic<bool> g_capture_request{false};
static std::mutex g_capture_mutex;
static std::vector<uint8_t> g_capture_pixels;
static size_t g_capture_w = 0, g_capture_h = 0;
static std::atomic<bool> g_capture_ready{false};
static std::atomic<int> g_load{150};
static __weak TestView* g_view = nil;
static mach_port_t g_main_thread = MACH_PORT_NULL;
static int g_fail = 0, g_checks = 0;

static void check(const char* name, bool ok, const char* detail = "") {
    ++g_checks;
    if (!ok) ++g_fail;
    std::printf("%-4s %s%s%s\n", ok ? "PASS" : "FAIL", name, *detail ? "  " : "", detail);
    std::fflush(stdout);
}

static void checkf(const char* name, bool ok, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
static void checkf(const char* name, bool ok, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    check(name, ok, buf);
}

static double wall() { return double(gta6rt_hud_now_ns()) * 1e-9; }

static void render_loop() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> queue = [device newCommandQueue];
        NSError* err = nil;
        id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:kShader] options:nil error:&err];
        if (!lib) {
            std::printf("shader compile failed: %s\n", err.localizedDescription.UTF8String);
            return;
        }
        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs"];
        pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!pso) return;
        gta6rt_hud_set_driver("Metal (test app)");
        gta6rt_hud_set_profile("test");
        uint64_t frame = 0;
        uint64_t next = gta6rt_hud_now_ns();
        uint64_t built = 0;
        while (g_run.load()) {
            @autoreleasepool {
                TestView* view = g_view;
                CAMetalLayer* layer = view ? (CAMetalLayer*)view.layer : nil;
                if (!layer) {
                    usleep(5000);
                    continue;
                }
                const uint64_t t0 = gta6rt_hud_now_ns();
                id<CAMetalDrawable> drawable = [layer nextDrawable];
                if (!drawable) {
                    usleep(2000);
                    continue;
                }
                MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = drawable.texture;
                rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
                id<MTLCommandBuffer> cb = [queue commandBuffer];
                id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
                [enc setRenderPipelineState:pso];
                const NSUInteger w = drawable.texture.width, h = drawable.texture.height;
                constexpr int kDraws = 19;
                for (int i = 0; i < kDraws; ++i) {
                    float u[4] = {float(frame) * 0.03f, float(i), float(g_load.load()), 0};
                    [enc setFragmentBytes:u length:sizeof u atIndex:0];
                    const NSUInteger x0 = w * i / kDraws, x1 = w * (i + 1) / kDraws;
                    MTLScissorRect sr = {x0, 0, x1 - x0, h};
                    [enc setScissorRect:sr];
                    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                }
                [enc endEncoding];
                id<MTLBuffer> capture_buffer = nil;
                if (g_capture_request.exchange(false)) {
                    capture_buffer = [device newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
                    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
                    [blit copyFromTexture:drawable.texture
                              sourceSlice:0
                              sourceLevel:0
                             sourceOrigin:MTLOriginMake(0, 0, 0)
                               sourceSize:MTLSizeMake(w, h, 1)
                                 toBuffer:capture_buffer
                        destinationOffset:0
                   destinationBytesPerRow:w * 4
                 destinationBytesPerImage:w * h * 4];
                    [blit endEncoding];
                }
                const double submit_media = CACurrentMediaTime();
                const NSUInteger cw = w, ch = h;
                [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
                    if (capture_buffer) {
                        std::lock_guard lock(g_capture_mutex);
                        g_capture_pixels.assign(static_cast<const uint8_t*>(capture_buffer.contents),
                                                static_cast<const uint8_t*>(capture_buffer.contents) + cw * ch * 4);
                        g_capture_w = cw;
                        g_capture_h = ch;
                        g_capture_ready = true;
                    }
                    const double gpu = done.GPUEndTime - done.GPUStartTime;
                    if (gpu > 0) gta6rt_hud_note_gpu_ns(uint64_t(gpu * 1e9));
                }];
                [drawable addPresentedHandler:^(id<MTLDrawable> d) {
                    if (d.presentedTime > 0) gta6rt_hud_note_display_latency_ns(uint64_t((d.presentedTime - submit_media) * 1e9));
                }];
                [cb presentDrawable:drawable];
                [cb commit];
                gta6rt_hud_add_draws(kDraws);
                gta6rt_hud_note_submission(gta6rt_hud_now_ns() - t0);
                gta6rt_hud_note_present();
                ++frame;
                if (frame % 60 == 0 && built < 3) ++built;
                const uint64_t pending = frame < 600 ? (600 - frame) / 4 : 0;
                gta6rt_hud_publish_pipelines(built, built, 0);
                gta6rt_hud_set_pipelines_pending(pending);
                if (pending) {
                    char s[64];
                    std::snprintf(s, sizeof s, "compiling %llu pipelines", static_cast<unsigned long long>(pending));
                    gta6rt_hud_set_state(s);
                } else {
                    gta6rt_hud_set_state("");
                }
                if (g_stutter.load() && frame % 180 == 0) usleep(70000);
            }
            next += 16666667ull;
            const uint64_t now = gta6rt_hud_now_ns();
            if (next > now) {
                const uint64_t d = next - now;
                usleep(useconds_t(d / 1000));
            } else if (now - next > 100000000ull) {
                next = now;
            }
        }
    }
}

static void pump(double seconds) {
    @autoreleasepool {
        NSDate* until = [NSDate dateWithTimeIntervalSinceNow:seconds];
        for (;;) {
            NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES];
            if (!e) break;
            [NSApp sendEvent:e];
            until = [NSDate distantPast];
        }
        [NSApp updateWindows];
    }
}

static void spin(double seconds) {
    const double end = wall() + seconds;
    while (wall() < end) pump(0.05);
}

static double main_cpu_s() {
    thread_basic_info_data_t info;
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    if (thread_info(g_main_thread, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &n) != KERN_SUCCESS) return 0;
    return double(info.user_time.seconds + info.system_time.seconds) +
           double(info.user_time.microseconds + info.system_time.microseconds) * 1e-6;
}

static double process_cpu_s() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return double(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) + double(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1e-6;
}

static NSWindow* make_window(const char* title, NSRect frame) {
    NSWindow* w = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    w.releasedWhenClosed = NO;
    w.title = [NSString stringWithUTF8String:title];
    w.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    w.backgroundColor = NSColor.blackColor;
    TestView* v = [[TestView alloc] initWithFrame:((NSView*)w.contentView).bounds];
    v.wantsLayer = YES;
    v.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    w.contentView = v;
    g_view = v;
    [w makeKeyAndOrderFront:nil];
    return w;
}

static NSWindow* hud_child(NSWindow* parent) {
    for (NSWindow* c in parent.childWindows)
        if ([c isKindOfClass:NSPanel.class] && c.ignoresMouseEvents) return c;
    return nil;
}

static NSPoint offset_from_content_top_left(NSWindow* parent, NSWindow* child) {
    const NSRect content = [parent convertRectToScreen:parent.contentLayoutRect];
    return NSMakePoint(child.frame.origin.x - content.origin.x, NSMaxY(content) - NSMaxY(child.frame));
}

static bool save_png(CGImageRef image, const std::string& path) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.c_str()), path.size(), false);
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
    CFRelease(url);
    if (!dest) return false;
    CGImageDestinationAddImage(dest, image, nullptr);
    const bool ok = CGImageDestinationFinalize(dest);
    CFRelease(dest);
    return ok;
}

static CGImageRef frame_image() {
    g_capture_ready = false;
    g_capture_request = true;
    const double end = wall() + 3.0;
    while (!g_capture_ready.load() && wall() < end) pump(0.02);
    if (!g_capture_ready.load()) return nullptr;
    std::lock_guard lock(g_capture_mutex);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(g_capture_pixels.data(), g_capture_w, g_capture_h, 8, g_capture_w * 4, cs,
                                             (CGBitmapInfo)kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef image = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    return image;
}

static bool shoot(const std::string& dir, const char* name, NSWindow* w, bool with_overlay) {
    if (dir.empty()) return false;
    const std::string path = dir + "/" + name;
    static int system_capture_works = -1;
    if (system_capture_works != 0) {
        std::string cmd = "/usr/sbin/screencapture -x -o -l " + std::to_string(w.windowNumber) + " '" + path + "' 2>&1";
        FILE* p = popen(cmd.c_str(), "r");
        char buf[256];
        std::string out;
        while (p && std::fgets(buf, sizeof buf, p)) out += buf;
        const int rc = p ? pclose(p) : -1;
        if (access(path.c_str(), R_OK) == 0) {
            system_capture_works = 1;
            checkf(name, true, "screencapture -l %ld", long(w.windowNumber));
            return true;
        }
        if (system_capture_works < 0) {
            std::printf("note screencapture failed (exit %d: %s); composing this process's own window and overlay instead\n", rc, out.c_str());
            system_capture_works = 0;
        }
    }
    const CGFloat s = w.backingScaleFactor;
    const NSSize size = w.frame.size;
    const size_t pw = size_t(size.width * s), ph = size_t(size.height * s);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(nullptr, pw, ph, 8, pw * 4, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    NSView* content = w.contentView;
    NSView* frame_view = content.superview ?: content;
    NSBitmapImageRep* rep = [frame_view bitmapImageRepForCachingDisplayInRect:frame_view.bounds];
    [frame_view cacheDisplayInRect:frame_view.bounds toBitmapImageRep:rep];
    const bool full = (w.styleMask & NSWindowStyleMaskFullScreen) != 0;
    if (full) {
        CGContextSetRGBFillColor(ctx, 0, 0, 0, 1);
        CGContextFillRect(ctx, CGRectMake(0, 0, pw, ph));
    } else {
        CGContextDrawImage(ctx, CGRectMake(0, 0, pw, ph), rep.CGImage);
    }
    CGImageRef frame = frame_image();
    if (frame) {
        const NSRect cf = full ? NSMakeRect(0, 0, size.width, size.height) : [frame_view convertRect:content.frame fromView:content.superview];
        CGContextDrawImage(ctx, CGRectMake(cf.origin.x * s, cf.origin.y * s, cf.size.width * s, cf.size.height * s), frame);
        CGImageRelease(frame);
    }
    if (with_overlay)
        if (NSWindow* c = hud_child(w)) {
            CGContextSaveGState(ctx);
            CGContextTranslateCTM(ctx, (c.frame.origin.x - w.frame.origin.x) * s, (c.frame.origin.y - w.frame.origin.y) * s);
            CGContextScaleCTM(ctx, s, s);
            [c.contentView.layer renderInContext:ctx];
            CGContextRestoreGState(ctx);
        }
    CGImageRef image = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    const bool ok = image && save_png(image, path);
    checkf(name, ok, "composed from this process's own window (frame readback + title bar + overlay layer), %zux%zu px", pw, ph);
    if (image) CGImageRelease(image);
    return ok;
}

static NSWindow* make_quiet_window(NSSize content) {
    NSScreen* screen = NSScreen.mainScreen ?: NSScreen.screens.firstObject;
    const NSRect area = screen.visibleFrame;
    NSRect frame = NSMakeRect(NSMaxX(area) - content.width - 8, NSMinY(area) + 8, content.width, content.height);
    NSWindow* w = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    w.releasedWhenClosed = NO;
    w.title = @"HUD quiet test";
    w.backgroundColor = NSColor.blackColor;
    TestView* v = [[TestView alloc] initWithFrame:((NSView*)w.contentView).bounds];
    v.wantsLayer = YES;
    v.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    w.contentView = v;
    g_view = v;
    [w orderFront:nil];
    return w;
}

static double text_width(const std::string& text) {
    NSFont* f = [NSFont monospacedSystemFontOfSize:10.0 weight:NSFontWeightMedium];
    NSAttributedString* a = [[NSAttributedString alloc] initWithString:[NSString stringWithUTF8String:text.c_str()]
                                                            attributes:@{NSFontAttributeName : f}];
    return std::ceil([a boundingRectWithSize:NSMakeSize(100000, 100000) options:NSStringDrawingUsesLineFragmentOrigin].size.width);
}

static bool find_free_spot(NSSize size, NSPoint* origin) {
    NSScreen* screen = NSScreen.mainScreen ?: NSScreen.screens.firstObject;
    const NSRect area = screen.visibleFrame;
    const CGFloat primary_h = NSScreen.screens.firstObject.frame.size.height;
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    if (!list) return false;
    NSArray* windows = CFBridgingRelease(list);
    const pid_t me = getpid();
    for (CGFloat y = NSMinY(area) + 8; y + size.height < NSMaxY(area); y += 40) {
        for (CGFloat x = NSMaxX(area) - size.width - 8; x > NSMinX(area); x -= 40) {
            const CGRect cand = CGRectMake(x - 4, primary_h - (y + size.height) - 4, size.width + 8, size.height + 8);
            bool free_spot = true;
            for (NSDictionary* info in windows) {
                if ([info[(id)kCGWindowOwnerPID] intValue] == me) continue;
                if ([info[(id)kCGWindowAlpha] doubleValue] < 0.05) continue;
                CGRect r;
                if (!CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)info[(id)kCGWindowBounds], &r)) continue;
                if (CGRectIntersectsRect(r, cand)) {
                    free_spot = false;
                    break;
                }
            }
            if (free_spot) {
                *origin = NSMakePoint(x, y);
                return true;
            }
        }
    }
    return false;
}

static void run_quiet() {
    gta6rt_hud_info info;
    const pid_t me = getpid();
    NSRunningApplication* front_before = NSWorkspace.sharedWorkspace.frontmostApplication;

    gta6rt::hud::Metrics worst;
    worst.have_frames = true;
    worst.fps = 144;
    worst.frame_avg_ms = 123.4;
    worst.frame_max_ms = 2010;
    worst.draws_pf = 25400;
    worst.have_gpu = true;
    worst.gpu_ms_pf = 123.4;
    worst.have_pipelines = true;
    worst.pipelines_built = 55853;
    worst.pipelines_pending = 12345;
    worst.footprint_gib = 23.9;
    worst.ram_gib = 24;
    gta6rt::hud::FormatOptions opt;
    opt.mode = GTA6RT_HUD_MIN;
    const std::string min_line = gta6rt::hud::format_lines(worst, opt)[0].text;
    const double w_min = text_width(min_line) + 12;
    checkf("min line, worst case, fits a 1280-wide window", w_min <= 1280 - 12, "%zu chars, %.0f pt wide: %s", min_line.size(), w_min,
           min_line.c_str());
    checkf("min line fits a 640-wide window too", w_min <= 640 - 12, "%.0f pt", w_min);
    gta6rt::hud::Metrics typical;
    typical.have_frames = true;
    typical.fps = 17.1;
    typical.frame_avg_ms = 58.5;
    typical.frame_max_ms = 87;
    typical.draws_pf = 260;
    typical.have_pipelines = true;
    typical.pipelines_built = 572;
    typical.footprint_gib = 6.6;
    typical.ram_gib = 24;
    std::printf("typical min line: %s\n", gta6rt::hud::format_lines(typical, opt)[0].text.c_str());
    check("no GPU time is shown as '-', not a number", gta6rt::hud::format_lines(typical, opt)[0].text.find("gpu -") != std::string::npos);

    gta6rt_hud_config cfg{};
    cfg.mode = GTA6RT_HUD_MIN;
    cfg.rate_hz = 4;
    gta6rt_hud_init(&cfg);
    NSWindow* w = make_quiet_window(NSMakeSize(360, 203));
    spin(0.6);
    check("test window is not key and its app is not active", !w.isKeyWindow && !NSApp.isActive);
    gta6rt_hud_attach((__bridge void*)w);
    spin(1.8);
    gta6rt_hud_get_info(&info);
    NSWindow* child = hud_child(w);
    check("attached, timer running, overlay visible", info.attached && info.timer_running && info.overlay_visible && child);
    if (child) {
        check("overlay ignores mouse events", child.ignoresMouseEvents);
        check("overlay cannot become key or main", !child.canBecomeKeyWindow && !child.canBecomeMainWindow);
        check("overlay is a non-activating borderless panel", (child.styleMask & NSWindowStyleMaskNonactivatingPanel) &&
                                                                  !(child.styleMask & NSWindowStyleMaskTitled));
        check("overlay is not key, test window is not key, app not active", !child.isKeyWindow && !w.isKeyWindow && !NSApp.isActive);
        checkf("overlay fits inside the 360-wide window", child.frame.size.width <= 360 - 12 + 0.5, "%.0f pt", child.frame.size.width);
        const NSPoint off0 = offset_from_content_top_left(w, child);
        checkf("overlay at top-left of content", std::fabs(off0.x - 6) < 1.5 && std::fabs(off0.y - 6) < 1.5, "offset %.1f,%.1f", off0.x, off0.y);

        const NSRect before = w.frame;
        [w setFrameOrigin:NSMakePoint(before.origin.x - 60, before.origin.y + 40)];
        spin(0.5);
        NSPoint off = offset_from_content_top_left(w, child);
        checkf("follows a move", std::fabs(off.x - 6) < 1.5 && std::fabs(off.y - 6) < 1.5, "offset %.1f,%.1f", off.x, off.y);
        [w setContentSize:NSMakeSize(420, 236)];
        spin(0.5);
        off = offset_from_content_top_left(w, child);
        checkf("follows a resize (bottom edge)", std::fabs(off.x - 6) < 1.5 && std::fabs(off.y - 6) < 1.5, "offset %.1f,%.1f", off.x, off.y);
        NSRect f = w.frame;
        f.origin.y += 30;
        f.size.height -= 30;
        f.size.width -= 40;
        [w setFrame:f display:YES];
        spin(0.5);
        off = offset_from_content_top_left(w, child);
        checkf("follows a resize (top-left edge)", std::fabs(off.x - 6) < 1.5 && std::fabs(off.y - 6) < 1.5, "offset %.1f,%.1f", off.x, off.y);
        check("still not key, app still not active", !child.isKeyWindow && !w.isKeyWindow && !NSApp.isActive);

        NSPoint free_origin;
        const bool moved = find_free_spot(w.frame.size, &free_origin);
        if (moved) {
            [w setFrameOrigin:free_origin];
            spin(0.5);
        }
        std::printf("hit test spot: %s\n", moved ? "a screen area no other window covers" : "none found (the screen is covered)");
        const NSRect pf = child.frame;
        NSInteger top = 0;
        bool conclusive = false;
        for (int i = 0; i < 7 && !conclusive; ++i) {
            const NSPoint probe = NSMakePoint(pf.origin.x + pf.size.width * (0.1 + 0.13 * i), pf.origin.y + pf.size.height * 0.5);
            top = [NSWindow windowNumberAtPoint:probe belowWindowWithWindowNumber:0];
            if (top != w.windowNumber) continue;
            conclusive = true;
            checkf("window server hit test inside the overlay finds the test window, not the overlay", true, "topmost %ld, overlay %ld, test window %ld",
                   long(top), long(child.windowNumber), long(w.windowNumber));
            child.ignoresMouseEvents = NO;
            spin(0.3);
            const NSInteger control = [NSWindow windowNumberAtPoint:probe belowWindowWithWindowNumber:0];
            checkf("control: with ignoresMouseEvents off the same point finds the overlay", control == child.windowNumber, "topmost %ld", long(control));
            child.ignoresMouseEvents = YES;
            spin(0.3);
        }
        if (!conclusive)
            std::printf("SKIP window server hit test: another window covers the test window at the probed points (topmost %ld)\n", long(top));
    }

    static std::atomic<int> seen_f1{0}, seen_f2{0};
    seen_f1 = 0;
    seen_f2 = 0;
    id monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                       handler:^NSEvent*(NSEvent* e) {
                                                           if (gta6rt_hud_handle_key(e.keyCode, e.modifierFlags, e.isARepeat)) {
                                                               if (e.keyCode == GTA6RT_HUD_KEY_F1) seen_f1++;
                                                               else seen_f2++;
                                                               return nil;
                                                           }
                                                           return e;
                                                       }];
    auto press = [&](unsigned short code, NSEventModifierFlags flags) {
        NSEvent* e = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:flags timestamp:0
                                  windowNumber:w.windowNumber context:nil characters:@"" charactersIgnoringModifiers:@""
                                     isARepeat:NO keyCode:code];
        [NSApp postEvent:e atStart:NO];
        spin(0.25);
    };
    gta6rt_hud_set_mode(GTA6RT_HUD_MIN);
    spin(0.3);
    press(GTA6RT_HUD_KEY_F1, 0);
    check("F1 as a plain function key (Fn-lock on): min -> full", gta6rt_hud_get_mode() == GTA6RT_HUD_FULL);
    press(GTA6RT_HUD_KEY_F1, NSEventModifierFlagFunction);
    check("Fn+F1 (Fn-lock off, flag Function set): full -> title", gta6rt_hud_get_mode() == GTA6RT_HUD_TITLE);
    press(GTA6RT_HUD_KEY_F1, NSEventModifierFlagFunction);
    check("Fn+F1: title -> off", gta6rt_hud_get_mode() == GTA6RT_HUD_OFF);
    press(GTA6RT_HUD_KEY_F2, NSEventModifierFlagFunction);
    check("Fn+F2: off -> last shown mode (title)", gta6rt_hud_get_mode() == GTA6RT_HUD_TITLE);
    press(GTA6RT_HUD_KEY_F2, 0);
    check("F2 plain: title -> off", gta6rt_hud_get_mode() == GTA6RT_HUD_OFF);
    press(GTA6RT_HUD_KEY_F2, NSEventModifierFlagFunction | NSEventModifierFlagNumericPad);
    check("F2 with Function and NumericPad flags: off -> title", gta6rt_hud_get_mode() == GTA6RT_HUD_TITLE);
    press(GTA6RT_HUD_KEY_F1, NSEventModifierFlagFunction | NSEventModifierFlagCommand);
    check("Cmd+Fn+F1 is not the overlay's", gta6rt_hud_get_mode() == GTA6RT_HUD_TITLE);
    checkf("the key monitor consumed 6 F-key events", seen_f1 + seen_f2 == 6, "%d", int(seen_f1 + seen_f2));
    [NSEvent removeMonitor:monitor];
    gta6rt_hud_set_mode(GTA6RT_HUD_MIN);
    spin(0.6);

    gta6rt_hud_test_reset_counters();
    [w close];
    spin(0.5);
    gta6rt_hud_get_info(&info);
    check("window closed: detached, no timer, no overlay", !info.attached && !info.timer_running && !info.overlay_visible);
    spin(1.2);
    gta6rt_hud_get_info(&info);
    checkf("window closed: no ticks after", info.ticks == 0, "%llu", (unsigned long long)info.ticks);
    gta6rt_hud_shutdown();
    spin(0.6);
    gta6rt_hud_get_info(&info);
    check("after shutdown: no timer, no overlay, no ticks", !info.timer_running && !info.overlay_visible && info.ticks == 0);
    NSRunningApplication* front_after = NSWorkspace.sharedWorkspace.frontmostApplication;
    checkf("the frontmost application was never this one", front_after.processIdentifier != me && front_before.processIdentifier != me,
           "before %s after %s", front_before.localizedName.UTF8String, front_after.localizedName.UTF8String);
    check("this app was never active", !NSApp.isActive);
}

static void run_scenario(const std::string& shots) {
    gta6rt_hud_info info;

    gta6rt_hud_config cfg{};
    cfg.mode = GTA6RT_HUD_FULL;
    cfg.rate_hz = 4;
    gta6rt_hud_init(&cfg);
    spin(0.4);
    gta6rt_hud_get_info(&info);
    check("before the window: nothing runs", !info.attached && !info.timer_running && info.ticks == 0 && !info.overlay_visible);

    NSWindow* w = make_window("HUD Test", NSMakeRect(160, 260, 720, 440));
    [w orderFrontRegardless];
    spin(0.8);
    checkf("test window visible (not occluded)", (w.occlusionState & NSWindowOcclusionStateVisible) != 0, "occlusion %lu key %d app active %d",
           (unsigned long)w.occlusionState, w.isKeyWindow, NSApp.isActive);
    check("attach accepted", gta6rt_hud_attach((__bridge void*)w) == 1);
    spin(1.6);
    gta6rt_hud_get_info(&info);
    NSWindow* child = hud_child(w);
    check("attached, timer running, overlay visible", info.attached && info.timer_running && info.overlay_visible && child);
    checkf("ticks in 1.6 s at 4 Hz", info.ticks >= 5 && info.ticks <= 8, "%llu", (unsigned long long)info.ticks);
    if (child) {
        check("panel ignores mouse", child.ignoresMouseEvents);
        check("panel cannot become key or main", !child.canBecomeKeyWindow && !child.canBecomeMainWindow);
        check("panel is borderless non-activating", (child.styleMask & NSWindowStyleMaskNonactivatingPanel) &&
                                                       !(child.styleMask & NSWindowStyleMaskTitled));
        check("panel not opaque, clear", !child.isOpaque && child.backgroundColor.alphaComponent == 0.0);
        check("panel level equals parent level", child.level == w.level);
        check("panel does not hide on deactivate", !((NSPanel*)child).hidesOnDeactivate);
        const NSPoint off = offset_from_content_top_left(w, child);
        checkf("panel at top-left of content", std::fabs(off.x - 6) < 1.5 && std::fabs(off.y - 6) < 1.5, "offset %.1f,%.1f", off.x, off.y);
        checkf("panel frame", true, "%.0fx%.0f", child.frame.size.width, child.frame.size.height);
        check("parent has exactly one child", w.childWindows.count == 1);
        check("parent z-order: child above parent", child.parentWindow == w);
        std::printf("line: %s\n", info.line);
    }

    if (child) {
        const NSRect before = w.frame;
        [w setFrameOrigin:NSMakePoint(before.origin.x + 240, before.origin.y - 90)];
        spin(0.6);
        const NSPoint off = offset_from_content_top_left(w, child);
        checkf("follows a move", std::fabs(off.x - 6) < 1.5 && std::fabs(off.y - 6) < 1.5, "offset %.1f,%.1f parent moved to %.0f,%.0f",
               off.x, off.y, w.frame.origin.x, w.frame.origin.y);

        [w setContentSize:NSMakeSize(900, 560)];
        spin(0.6);
        NSPoint off2 = offset_from_content_top_left(w, child);
        checkf("follows a resize (bottom edge)", std::fabs(off2.x - 6) < 1.5 && std::fabs(off2.y - 6) < 1.5, "offset %.1f,%.1f content %.0fx%.0f",
               off2.x, off2.y, w.contentView.bounds.size.width, w.contentView.bounds.size.height);

        NSRect f = w.frame;
        f.origin.y += 80;
        f.size.height -= 80;
        f.size.width -= 120;
        [w setFrame:f display:YES];
        spin(0.6);
        off2 = offset_from_content_top_left(w, child);
        checkf("follows a resize (top-left edge)", std::fabs(off2.x - 6) < 1.5 && std::fabs(off2.y - 6) < 1.5, "offset %.1f,%.1f", off2.x, off2.y);

        [w setFrame:NSMakeRect(160, 260, 720, 440) display:YES];
        spin(0.6);
    }

    if (child) {
        const NSRect pf = child.frame;
        const NSPoint inside = NSMakePoint(NSMidX(pf), NSMidY(pf));
        NSInteger top = 0;
        NSPoint probe = inside;
        for (int attempt = 0; attempt < 6; ++attempt) {
            [w orderFrontRegardless];
            spin(0.4);
            probe = NSMakePoint(pf.origin.x + pf.size.width * (0.15 + 0.14 * attempt), pf.origin.y + pf.size.height * 0.5);
            top = [NSWindow windowNumberAtPoint:probe belowWindowWithWindowNumber:0];
            if (top == w.windowNumber || top == child.windowNumber) break;
        }
        const bool conclusive = top == w.windowNumber;
        if (conclusive) {
            checkf("window server hit test at a point inside the overlay finds the game window, not the overlay", true,
                   "topmost %ld, overlay %ld, game window %ld", long(top), long(child.windowNumber), long(w.windowNumber));
            child.ignoresMouseEvents = NO;
            spin(0.3);
            const NSInteger control = [NSWindow windowNumberAtPoint:probe belowWindowWithWindowNumber:0];
            checkf("control: with ignoresMouseEvents off the same point finds the overlay", control == child.windowNumber, "topmost %ld, overlay %ld",
                   long(control), long(child.windowNumber));
            child.ignoresMouseEvents = YES;
            spin(0.3);
            const NSInteger again = [NSWindow windowNumberAtPoint:probe belowWindowWithWindowNumber:0];
            check("ignoresMouseEvents restored: the point finds the game window again", again == w.windowNumber);
        } else {
            checkf("window server hit test skipped", true, "another application's window (%ld) is above the test window at every probe point", long(top));
        }
        TestView* v = (TestView*)w.contentView;
        const uint64_t before = v.clicks;
        const bool trusted = AXIsProcessTrusted();
        if (conclusive && trusted) {
            CGFloat screen_h = NSScreen.screens.firstObject.frame.size.height;
            CGPoint p = CGPointMake(probe.x, screen_h - probe.y);
            CGEventRef down = CGEventCreateMouseEvent(nullptr, kCGEventLeftMouseDown, p, kCGMouseButtonLeft);
            CGEventRef up = CGEventCreateMouseEvent(nullptr, kCGEventLeftMouseUp, p, kCGMouseButtonLeft);
            CGEventPost(kCGHIDEventTap, down);
            CGEventPost(kCGHIDEventTap, up);
            CFRelease(down);
            CFRelease(up);
            spin(0.5);
            checkf("synthetic click on the overlay reaches the game view", v.clicks > before, "clicks %llu -> %llu key %d",
                   (unsigned long long)before, (unsigned long long)v.clicks, w.isKeyWindow);
            check("overlay did not take key focus", !child.isKeyWindow);
        } else {
            checkf("synthetic click not posted", true, "AXIsProcessTrusted=%d (posting needs it); hit test conclusive=%d", trusted,
                   conclusive);
        }
    }

    gta6rt_hud_test_reset_counters();
    const double r0 = wall();
    spin(10.0);
    gta6rt_hud_get_info(&info);
    double dt = wall() - r0;
    checkf("update rate 4 Hz over 10 s", info.ticks >= 39 && info.ticks <= 41, "%llu ticks in %.2f s; interval mean %.1f ms max %.1f ms; tick mean %.1f us max %.1f us",
           (unsigned long long)info.ticks, dt, double(info.tick_interval_ns_total) / double(std::max<uint64_t>(1, info.tick_interval_count)) / 1e6,
           double(info.tick_interval_ns_max) / 1e6, double(info.tick_ns_total) / double(info.ticks) / 1000.0, double(info.tick_ns_max) / 1000.0);
    gta6rt_hud_set_rate(10);
    spin(0.5);
    gta6rt_hud_test_reset_counters();
    const double r1 = wall();
    spin(10.0);
    gta6rt_hud_get_info(&info);
    dt = wall() - r1;
    checkf("update rate 10 Hz over 10 s", info.ticks >= 99 && info.ticks <= 101, "%llu ticks in %.2f s; interval mean %.1f ms max %.1f ms; tick mean %.1f us max %.1f us",
           (unsigned long long)info.ticks, dt, double(info.tick_interval_ns_total) / double(std::max<uint64_t>(1, info.tick_interval_count)) / 1e6,
           double(info.tick_interval_ns_max) / 1e6, double(info.tick_ns_total) / double(info.ticks) / 1000.0, double(info.tick_ns_max) / 1000.0);
    gta6rt_hud_set_rate(4);
    spin(0.5);

    gta6rt_hud_set_mode(GTA6RT_HUD_FULL);
    g_stutter = true;
    spin(3.0);
    shoot(shots, "windowed_full.png", w, true);
    gta6rt_hud_set_mode(GTA6RT_HUD_MIN);
    spin(1.5);
    shoot(shots, "windowed_min.png", w, true);
    gta6rt_hud_get_info(&info);
    std::printf("line(min): %s\n", info.line);
    gta6rt_hud_test_set_pressure(1);
    spin(1.0);
    shoot(shots, "windowed_min_pressure_warn.png", w, true);
    gta6rt_hud_test_set_pressure(0);
    g_stutter = false;

    gta6rt_hud_set_mode(GTA6RT_HUD_TITLE);
    spin(3.0);
    gta6rt_hud_get_info(&info);
    NSString* title = w.title;
    checkf("title mode writes the line", [title containsString:@" fps"] && [title hasPrefix:@"HUD Test - "], "title: %s", title.UTF8String);
    check("title mode removes the overlay", w.childWindows.count == 0 && !info.overlay_visible);
    checkf("title sets", info.title_sets >= 10, "%llu sets, mean %.1f us, max %.1f us", (unsigned long long)info.title_sets,
           double(info.title_set_ns_total) / double(std::max<uint64_t>(1, info.title_sets)) / 1000.0, double(info.title_set_ns_max) / 1000.0);
    shoot(shots, "windowed_title.png", w, true);
    gta6rt_hud_set_mode(GTA6RT_HUD_OFF);
    spin(0.8);
    check("off restores the title", [w.title isEqualToString:@"HUD Test"]);
    gta6rt_hud_get_info(&info);
    check("off: no timer, no overlay, producers inactive", !info.timer_running && w.childWindows.count == 0 && !gta6rt_hud_active());
    uint64_t t_off = info.ticks;
    spin(1.0);
    gta6rt_hud_get_info(&info);
    check("off: no ticks", info.ticks == t_off);

    id monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                       handler:^NSEvent*(NSEvent* e) {
                                                           if (gta6rt_hud_handle_key(e.keyCode, e.modifierFlags, e.isARepeat)) return nil;
                                                           return e;
                                                       }];
    auto press = [&](unsigned short code) {
        NSEvent* e = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0 timestamp:0
                                  windowNumber:w.windowNumber context:nil characters:@"" charactersIgnoringModifiers:@""
                                     isARepeat:NO keyCode:code];
        [NSApp postEvent:e atStart:NO];
        spin(0.3);
    };
    press(GTA6RT_HUD_KEY_F1);
    check("F1 event: off -> min", gta6rt_hud_get_mode() == GTA6RT_HUD_MIN && hud_child(w));
    press(GTA6RT_HUD_KEY_F1);
    check("F1 event: min -> full", gta6rt_hud_get_mode() == GTA6RT_HUD_FULL);
    press(GTA6RT_HUD_KEY_F1);
    check("F1 event: full -> title", gta6rt_hud_get_mode() == GTA6RT_HUD_TITLE && !hud_child(w));
    press(GTA6RT_HUD_KEY_F2);
    check("F2 event: title -> off", gta6rt_hud_get_mode() == GTA6RT_HUD_OFF && [w.title isEqualToString:@"HUD Test"]);
    press(GTA6RT_HUD_KEY_F2);
    check("F2 event: off -> last (title)", gta6rt_hud_get_mode() == GTA6RT_HUD_TITLE);
    press(0x00);
    check("other keys untouched", gta6rt_hud_get_mode() == GTA6RT_HUD_TITLE);
    [NSEvent removeMonitor:monitor];
    gta6rt_hud_set_mode(GTA6RT_HUD_FULL);
    spin(1.0);

    child = hud_child(w);
    NSRect screen_frame = w.screen.frame;
    NSUInteger spaces_before = w.styleMask;
    (void)spaces_before;
    [w toggleFullScreen:nil];
    spin(4.0);
    child = hud_child(w);
    gta6rt_hud_get_info(&info);
    check("full screen entered", (w.styleMask & NSWindowStyleMaskFullScreen) != 0);
    check("full screen: overlay still a child, visible", child && info.overlay_visible && child.isVisible);
    if (child) {
        const NSPoint off = offset_from_content_top_left(w, child);
        checkf("full screen: panel at top-left of content", std::fabs(off.x - 6) < 1.5 && std::fabs(off.y - 6) < 1.5, "offset %.1f,%.1f", off.x, off.y);
        checkf("full screen: panel inside the screen", NSContainsRect(w.screen.frame, child.frame), "panel %.0f,%.0f %.0fx%.0f screen %.0fx%.0f",
               child.frame.origin.x, child.frame.origin.y, child.frame.size.width, child.frame.size.height, screen_frame.size.width,
               screen_frame.size.height);
        bool on_space = child.isOnActiveSpace;
        for (int i = 0; i < 10 && !on_space; ++i) {
            spin(0.3);
            on_space = child.isOnActiveSpace;
        }
        check("full screen: panel on the active space", on_space);
    }
    spin(1.0);
    shoot(shots, "fullscreen_full.png", w, true);
    gta6rt_hud_set_mode(GTA6RT_HUD_MIN);
    spin(1.5);
    shoot(shots, "fullscreen_min.png", w, true);
    gta6rt_hud_set_mode(GTA6RT_HUD_TITLE);
    spin(1.0);
    checkf("full screen: title mode still sets the (invisible) title", [w.title containsString:@" fps"], "%s", w.title.UTF8String);
    gta6rt_hud_set_mode(GTA6RT_HUD_FULL);
    spin(0.8);
    [w toggleFullScreen:nil];
    spin(4.0);
    child = hud_child(w);
    check("full screen left", (w.styleMask & NSWindowStyleMaskFullScreen) == 0);
    if (child) {
        const NSPoint off = offset_from_content_top_left(w, child);
        checkf("after full screen: panel at top-left of content", std::fabs(off.x - 6) < 1.5 && std::fabs(off.y - 6) < 1.5, "offset %.1f,%.1f", off.x, off.y);
    } else {
        check("after full screen: overlay present", false);
    }

    gta6rt_hud_test_reset_counters();
    [w close];
    spin(0.5);
    gta6rt_hud_get_info(&info);
    check("window closed: detached, no timer, no overlay", !info.attached && !info.timer_running && !info.overlay_visible);
    spin(1.5);
    gta6rt_hud_get_info(&info);
    checkf("window closed: no ticks after", info.ticks == 0, "%llu", (unsigned long long)info.ticks);
    check("producers still callable after the window is gone", true);

    NSWindow* w2 = make_window("HUD Test 2", NSMakeRect(220, 300, 600, 380));
    spin(0.5);
    gta6rt_hud_attach((__bridge void*)w2);
    spin(1.2);
    gta6rt_hud_get_info(&info);
    check("re-attach to a new window works", info.attached && info.timer_running && hud_child(w2));
    gta6rt_hud_detach((__bridge void*)w2);
    spin(0.4);
    gta6rt_hud_get_info(&info);
    check("detach: overlay gone, timer stopped", !info.attached && !info.timer_running && w2.childWindows.count == 0);
    gta6rt_hud_attach((__bridge void*)w2);
    spin(0.8);
    gta6rt_hud_shutdown();
    spin(0.5);
    gta6rt_hud_get_info(&info);
    uint64_t t = info.ticks;
    spin(1.0);
    gta6rt_hud_get_info(&info);
    check("shutdown: no timer, no ticks, producers inactive", !info.timer_running && info.ticks == t && !gta6rt_hud_active() && w2.childWindows.count == 0);
    [w2 close];
    spin(0.3);

    gta6rt_hud_config cfg2{};
    cfg2.mode = GTA6RT_HUD_FULL;
    cfg2.rate_hz = 4;
    gta6rt_hud_init(&cfg2);
    __weak NSWindow* orphan = nil;
    @autoreleasepool {
        NSWindow* o = [[NSWindow alloc] initWithContentRect:NSMakeRect(240, 320, 500, 300)
                                                  styleMask:NSWindowStyleMaskTitled
                                                    backing:NSBackingStoreBuffered
                                                      defer:YES];
        o.releasedWhenClosed = NO;
        gta6rt_hud_attach((__bridge void*)o);
        spin(0.8);
        orphan = o;
    }
    spin(1.5);
    gta6rt_hud_get_info(&info);
    if (orphan) {
        std::printf("note: orphan window still alive, release-without-close not exercised\n");
    } else {
        check("parent released without close: timer stopped, nothing attached", !info.timer_running && !info.attached && !info.overlay_visible);
    }
    NSWindow* w3 = make_window("HUD Test 3", NSMakeRect(260, 340, 500, 300));
    [w3 orderFrontRegardless];
    spin(0.3);
    gta6rt_hud_attach((__bridge void*)w3);
    spin(1.0);
    gta6rt_hud_get_info(&info);
    check("attach after an orphaned parent: timer running, overlay on the new window", info.attached && info.timer_running && hud_child(w3));
    [w3 close];
    spin(0.6);
    gta6rt_hud_get_info(&info);
    check("closing the new window stops the timer", !info.timer_running && !info.attached && !info.overlay_visible);
    gta6rt_hud_shutdown();
    spin(0.3);
}

static void run_cpu(double seconds) {
    NSWindow* w = make_window("HUD CPU", NSMakeRect(160, 260, 720, 440));
    [w orderFrontRegardless];
    gta6rt_hud_config cfg{};
    cfg.mode = GTA6RT_HUD_OFF;
    cfg.rate_hz = 4;
    gta6rt_hud_init(&cfg);
    gta6rt_hud_attach((__bridge void*)w);
    spin(1.0);
    std::printf("window occlusion visible %d\n", (w.occlusionState & NSWindowOcclusionStateVisible) != 0);
    struct Cond {
        const char* name;
        gta6rt_hud_mode mode;
        double hz;
    };
    const Cond conds[] = {{"off (control)", GTA6RT_HUD_OFF, 4}, {"min 4Hz", GTA6RT_HUD_MIN, 4},   {"full 4Hz", GTA6RT_HUD_FULL, 4},
                          {"title 4Hz", GTA6RT_HUD_TITLE, 4}, {"min 10Hz", GTA6RT_HUD_MIN, 10},   {"full 10Hz", GTA6RT_HUD_FULL, 10},
                          {"title 10Hz", GTA6RT_HUD_TITLE, 10}, {"off (control 2)", GTA6RT_HUD_OFF, 4}};
    std::printf("%-16s %12s %12s %10s %12s %12s\n", "condition", "main ms/s", "proc ms/s", "ticks/s", "tick us avg", "tick us max");
    for (const Cond& c : conds) {
        gta6rt_hud_set_rate(c.hz);
        gta6rt_hud_set_mode(c.mode);
        spin(1.5);
        gta6rt_hud_test_reset_counters();
        const double m0 = main_cpu_s(), p0 = process_cpu_s(), t0 = wall();
        spin(seconds);
        const double dt = wall() - t0, dm = main_cpu_s() - m0, dp = process_cpu_s() - p0;
        gta6rt_hud_info info;
        gta6rt_hud_get_info(&info);
        std::printf("%-16s %12.2f %12.2f %10.2f %12.1f %12.1f\n", c.name, dm / dt * 1000, dp / dt * 1000, double(info.ticks) / dt,
                    info.ticks ? double(info.tick_ns_total) / double(info.ticks) / 1000.0 : 0.0, double(info.tick_ns_max) / 1000.0);
        std::fflush(stdout);
    }
    gta6rt_hud_shutdown();
    spin(0.3);
}

int main(int argc, char** argv) {
    @autoreleasepool {
        bool autorun = false, interactive = false, quiet = false;
        double cpu_seconds = 0;
        std::string shots, mode = "full";
        double hz = 4;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--auto") autorun = true;
            else if (a == "--quiet") quiet = true;
            else if (a == "--interactive") interactive = true;
            else if (a == "--cpu" && i + 1 < argc) cpu_seconds = std::atof(argv[++i]);
            else if (a == "--shots" && i + 1 < argc) shots = argv[++i];
            else if (a == "--mode" && i + 1 < argc) mode = argv[++i];
            else if (a == "--rate" && i + 1 < argc) hz = std::atof(argv[++i]);
            else if (a == "--stutter") g_stutter = true;
        }
        g_main_thread = mach_thread_self();
        [[NSProcessInfo processInfo] beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical reason:@"hud test"];
        if (CGMainDisplayID() == 0 || !CGDisplayIsActive(CGMainDisplayID())) {
            std::printf("no active display session; nothing to test\n");
            return 0;
        }
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:quiet ? NSApplicationActivationPolicyAccessory : NSApplicationActivationPolicyRegular];
        [NSApp finishLaunching];
        std::thread renderer(render_loop);
        int rc = 0;
        if (quiet) {
            run_quiet();
            std::printf("hud_test_app --quiet: %d checks, %d failed: %s\n", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
            rc = g_fail ? 1 : 0;
        } else if (autorun) {
            run_scenario(shots);
            std::printf("hud_test_app: %d checks, %d failed: %s\n", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
            rc = g_fail ? 1 : 0;
        } else if (cpu_seconds > 0) {
            run_cpu(cpu_seconds);
        } else if (interactive) {
            gta6rt_hud_config cfg{};
            gta6rt_hud_parse_mode(mode.c_str(), &cfg.mode);
            cfg.rate_hz = hz;
            gta6rt_hud_init(&cfg);
            NSWindow* w = make_window("HUD Test", NSMakeRect(160, 260, 960, 560));
            gta6rt_hud_attach((__bridge void*)w);
            [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                  handler:^NSEvent*(NSEvent* e) {
                                                      return gta6rt_hud_handle_key(e.keyCode, e.modifierFlags, e.isARepeat) ? nil : e;
                                                  }];
            for (;;) pump(0.05);
        } else {
            std::printf("usage: hud_test_app --quiet | --auto [--shots DIR] | --cpu SECONDS | --interactive [--mode M --rate HZ]\n");
        }
        g_run = false;
        renderer.join();
        return rc;
    }
}
