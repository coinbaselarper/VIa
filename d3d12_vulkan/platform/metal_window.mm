#include "mac_window.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

struct gta6rt_mac_window {
    NSWindow* window = nil;
    CAMetalLayer* layer = nil;
    id delegate = nil;
    std::atomic<uint32_t> width{0}, height{0};
    std::atomic<bool> visible{false};
    std::atomic<bool> close_requested{false};
};

namespace {

std::mutex g_registry_mutex;
std::vector<gta6rt_mac_window*> g_registry;
std::atomic<uint32_t> g_main_wait_ms{5000};

bool registered(const gta6rt_mac_window* w) {
    std::lock_guard lock(g_registry_mutex);
    return std::find(g_registry.begin(), g_registry.end(), w) != g_registry.end();
}

void ensure_application() {
    static bool done = false;
    if (done) return;
    done = true;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp finishLaunching];
}

void run_on_main(void (^block)(void)) {
    if ([NSThread isMainThread]) block();
    else dispatch_async(dispatch_get_main_queue(), block);
}

}  // namespace

@interface Gta6rtMetalView : NSView
@property(nonatomic, assign) gta6rt_mac_window* owner;
@end

@implementation Gta6rtMetalView
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer*)makeBackingLayer { return [CAMetalLayer layer]; }
- (void)updateDrawableSize {
    CAMetalLayer* layer = (CAMetalLayer*)self.layer;
    const CGFloat scale = self.window ? self.window.backingScaleFactor : 1.0;
    layer.contentsScale = scale;
    const NSSize points = self.bounds.size;
    const CGSize pixels = CGSizeMake(points.width * scale, points.height * scale);
    layer.drawableSize = pixels;
    if (_owner) {
        _owner->width.store(uint32_t(pixels.width));
        _owner->height.store(uint32_t(pixels.height));
    }
}
- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self updateDrawableSize];
}
- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    [self updateDrawableSize];
}
- (BOOL)acceptsFirstResponder { return YES; }
@end

@interface Gta6rtWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) gta6rt_mac_window* owner;
@end

@implementation Gta6rtWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    if (_owner) _owner->close_requested.store(true);
    return NO;
}
- (void)windowDidMiniaturize:(NSNotification*)n {
    (void)n;
    if (_owner) _owner->visible.store(false);
}
- (void)windowDidDeminiaturize:(NSNotification*)n {
    (void)n;
    if (_owner) _owner->visible.store(true);
}
@end

namespace {

void build_visible(gta6rt_mac_window* w, NSString* title, uint32_t width, uint32_t height) {
    ensure_application();
    NSScreen* screen = [NSScreen mainScreen];
    const CGFloat scale = screen ? screen.backingScaleFactor : 1.0;
    const NSRect content = NSMakeRect(0, 0, width / scale, height / scale);
    NSWindow* window = [[NSWindow alloc]
        initWithContentRect:content
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    window.releasedWhenClosed = NO;
    window.title = title;
    Gta6rtMetalView* view = [[Gta6rtMetalView alloc] initWithFrame:content];
    view.owner = w;
    view.wantsLayer = YES;
    view.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;
    window.contentView = view;
    Gta6rtWindowDelegate* delegate = [Gta6rtWindowDelegate new];
    delegate.owner = w;
    window.delegate = delegate;
    CAMetalLayer* layer = (CAMetalLayer*)view.layer;
    layer.device = MTLCreateSystemDefaultDevice();
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = NO;
    [view updateDrawableSize];
    [window center];
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    w->window = window;
    w->layer = layer;
    w->delegate = delegate;
    w->visible.store(true);
}

void build_layer_only(gta6rt_mac_window* w, uint32_t width, uint32_t height) {
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.device = MTLCreateSystemDefaultDevice();
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = NO;
    layer.contentsScale = 1.0;
    layer.frame = CGRectMake(0, 0, width, height);
    layer.drawableSize = CGSizeMake(width, height);
    w->layer = layer;
    w->width.store(width);
    w->height.store(height);
}

}  // namespace

extern "C" {

gta6rt_mac_window* gta6rt_mac_window_create(const char* title_utf8, uint32_t width, uint32_t height, int visible) {
    if (!width || !height) return nullptr;
    auto* w = new gta6rt_mac_window;
    @autoreleasepool {
        NSString* title = [NSString stringWithUTF8String:title_utf8 ? title_utf8 : ""];
        if (!visible) {
            build_layer_only(w, width, height);
        } else if ([NSThread isMainThread]) {
            build_visible(w, title, width, height);
        } else {
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            dispatch_async(dispatch_get_main_queue(), ^{
                build_visible(w, title, width, height);
                dispatch_semaphore_signal(done);
            });
            const uint32_t ms = g_main_wait_ms.load();
            if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, int64_t(ms) * NSEC_PER_MSEC))) {
                std::fprintf(stderr,
                             "[rt] mac_window: the main queue did not answer in %u ms; '%s' is a hidden layer-only "
                             "window (the host's main thread must service the main dispatch queue)\n",
                             ms, title_utf8 ? title_utf8 : "");
                // The queued block still runs later; it must not touch this window, so it gets its own.
                auto* orphan = w;
                w = new gta6rt_mac_window;
                build_layer_only(w, width, height);
                (void)orphan;
            }
        }
    }
    if (!w->layer) {
        delete w;
        return nullptr;
    }
    std::lock_guard lock(g_registry_mutex);
    g_registry.push_back(w);
    return w;
}

void gta6rt_mac_window_destroy(gta6rt_mac_window* w) {
    {
        std::lock_guard lock(g_registry_mutex);
        auto it = std::find(g_registry.begin(), g_registry.end(), w);
        if (it == g_registry.end()) return;
        g_registry.erase(it);
    }
    NSWindow* window = w->window;
    w->window = nil;
    w->delegate = nil;
    w->layer = nil;
    delete w;
    if (window) run_on_main(^{
        window.delegate = nil;
        [window orderOut:nil];
        [window close];
    });
}

int gta6rt_mac_window_alive(const gta6rt_mac_window* w) { return w && registered(w); }

void* gta6rt_mac_window_metal_layer(gta6rt_mac_window* w) {
    return gta6rt_mac_window_alive(w) ? (__bridge void*)w->layer : nullptr;
}

int gta6rt_mac_window_client_pixels(const gta6rt_mac_window* w, uint32_t* width, uint32_t* height) {
    if (!gta6rt_mac_window_alive(w)) return 0;
    const uint32_t cw = w->width.load(), ch = w->height.load();
    if (width) *width = cw;
    if (height) *height = ch;
    return cw && ch;
}

int gta6rt_mac_window_visible(const gta6rt_mac_window* w) { return gta6rt_mac_window_alive(w) && w->visible.load(); }

void gta6rt_mac_window_set_title(gta6rt_mac_window* w, const char* title_utf8) {
    if (!gta6rt_mac_window_alive(w) || !w->window) return;
    NSWindow* window = w->window;
    NSString* title = [NSString stringWithUTF8String:title_utf8 ? title_utf8 : ""];
    run_on_main(^{ window.title = title; });
}

void gta6rt_mac_window_set_client_size(gta6rt_mac_window* w, uint32_t width, uint32_t height) {
    if (!gta6rt_mac_window_alive(w) || !width || !height) return;
    if (!w->window) {
        CAMetalLayer* layer = w->layer;
        layer.drawableSize = CGSizeMake(width, height);
        w->width.store(width);
        w->height.store(height);
        return;
    }
    NSWindow* window = w->window;
    run_on_main(^{
        const CGFloat scale = window.backingScaleFactor;
        [window setContentSize:NSMakeSize(width / scale, height / scale)];
    });
}

int gta6rt_mac_window_close_requested(const gta6rt_mac_window* w) {
    return gta6rt_mac_window_alive(w) && w->close_requested.load();
}

gta6rt_mac_window* gta6rt_mac_window_find_largest(const gta6rt_mac_window* exclude) {
    std::lock_guard lock(g_registry_mutex);
    gta6rt_mac_window* best = nullptr;
    uint64_t best_area = 0;
    for (auto* w : g_registry) {
        if (w == exclude || !w->visible.load()) continue;
        const uint64_t area = uint64_t(w->width.load()) * w->height.load();
        if (!best || area > best_area) best = w, best_area = area;
    }
    return best;
}

int gta6rt_mac_window_pump_events(double timeout_seconds) {
    if (![NSThread isMainThread]) return 0;
    ensure_application();
    int handled = 0;
    @autoreleasepool {
        NSDate* until = [NSDate dateWithTimeIntervalSinceNow:std::max(0.0, timeout_seconds)];
        for (;;) {
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:until
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
            if (!event) break;
            [NSApp sendEvent:event];
            ++handled;
            until = [NSDate distantPast];
        }
        [NSApp updateWindows];
    }
    return handled;
}

void gta6rt_mac_window_set_main_wait_ms(uint32_t ms) { g_main_wait_ms.store(ms); }

}  // extern "C"
