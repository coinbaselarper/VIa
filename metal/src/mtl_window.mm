#include "mtl_internal.h"

#import <AppKit/AppKit.h>

@interface Gta6rtMetalLayerView : NSView
@end

@implementation Gta6rtMetalLayerView
- (BOOL)wantsUpdateLayer {
    return YES;
}
- (CALayer*)makeBackingLayer {
    return [CAMetalLayer layer];
}
@end

namespace gta6rt::metal {

namespace {

class MacWindow final : public Window {
public:
    MacWindow(NSWindow* window, CAMetalLayer* layer, std::uint32_t w, std::uint32_t h)
        : window_(window), layer_(layer), width_(w), height_(h) {}
    ~MacWindow() override {
        if (window_) [window_ close];
    }
    void* layer() override { return (__bridge void*)layer_; }
    bool visible() const override { return window_ != nil && window_.visible; }
    std::uint32_t width() const override { return width_; }
    std::uint32_t height() const override { return height_; }
    int pump_events(double seconds) override {
        if (![NSThread isMainThread] || !NSApp) return 0;
        int n = 0;
        NSDate* until = [NSDate dateWithTimeIntervalSinceNow:seconds];
        for (;;) {
            @autoreleasepool {
                NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:until
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
                if (!e) break;
                [NSApp sendEvent:e];
                ++n;
            }
        }
        return n;
    }

private:
    NSWindow* window_;
    CAMetalLayer* layer_;
    std::uint32_t width_, height_;
};

}  // namespace

std::unique_ptr<Window> create_window(const char* title, std::uint32_t width, std::uint32_t height, bool visible) {
    if (!width || !height) return nullptr;
    @autoreleasepool {
        if (!visible || ![NSThread isMainThread]) {
            CAMetalLayer* layer = [CAMetalLayer layer];
            layer.contentsScale = 1.0;
            layer.frame = CGRectMake(0, 0, width, height);
            layer.drawableSize = CGSizeMake(width, height);
            return std::make_unique<MacWindow>(nil, layer, width, height);
        }
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp finishLaunching];
        NSScreen* screen = [NSScreen mainScreen];
        const CGFloat scale = screen ? screen.backingScaleFactor : 1.0;
        const NSRect content = NSMakeRect(0, 0, width / scale, height / scale);
        NSWindow* window = [[NSWindow alloc] initWithContentRect:content
                                                       styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        window.releasedWhenClosed = NO;
        window.title = @(title ? title : "gta6rt_metal");
        Gta6rtMetalLayerView* view = [[Gta6rtMetalLayerView alloc] initWithFrame:content];
        view.wantsLayer = YES;
        window.contentView = view;
        CAMetalLayer* layer = (CAMetalLayer*)view.layer;
        layer.contentsScale = scale;
        layer.drawableSize = CGSizeMake(width, height);
        [window center];
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        return std::make_unique<MacWindow>(window, layer, width, height);
    }
}

}  // namespace gta6rt::metal
