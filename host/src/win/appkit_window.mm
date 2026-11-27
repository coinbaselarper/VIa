#include "appkit_window.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "gta6rt_hud/hud.h"
#include "win_core.h"

extern "C" {
void gta6rt_gameinput_mouse_motion(double dx, double dy_up);
void gta6rt_gameinput_mouse_buttons(int left, int right);
void gta6rt_gameinput_mouse_release(void);
int gta6rt_gameinput_mouse_active(void);
}

namespace {

constexpr NSWindowStyleMask kHostStyle = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                         NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;

struct Entry {
    gta6rt_mac_window* window = nullptr;
    NSWindow* ns = nil;
    host_win_event_fn fn = nullptr;
    void* ctx = nullptr;
    NSMutableArray* tokens = nil;
    std::atomic<bool> fullscreen{false};
    std::atomic<bool> minimized{false};
    std::atomic<int32_t> x{0}, y{0};
    std::atomic<uint32_t> w{0}, h{0};
    uint32_t logical_w = 0, logical_h = 0;
    bool zoomed_by_us = false;
};

std::mutex g_mu;
std::vector<std::shared_ptr<Entry>> g_entries;
bool g_app_installed = false;

struct Capture {
    bool captured = false;
    bool option_down = false;
    bool off = false;
    CFAbsoluteTime key_since = 0;
    NSTimer* timer = nil;
} g_cap;

void release_cursor() {
    if (!g_cap.captured) return;
    g_cap.captured = false;
    CGAssociateMouseAndMouseCursorPosition(true);
    [NSCursor unhide];
    std::fprintf(stderr, "[rt] host_win: mouse released\n");
}

NSWindow* key_host_window();

void capture_cursor(NSWindow* ns) {
    if (g_cap.captured || g_cap.off || g_cap.option_down) return;
    if (!ns.isKeyWindow || !NSApp.isActive || ns.isMiniaturized || !ns.isVisible) return;
    if (!gta6rt_gameinput_mouse_active() || NSEvent.pressedMouseButtons != 0) return;
    NSView* view = ns.contentView;
    const NSRect in_window = [view convertRect:view.bounds toView:nil];
    const NSRect on_screen = [ns convertRectToScreen:in_window];
    const CGFloat primary_h = NSScreen.screens.firstObject.frame.size.height;
    if (!NSPointInRect(NSEvent.mouseLocation, NSInsetRect(on_screen, 40, 40)))
        CGWarpMouseCursorPosition(CGPointMake(NSMidX(on_screen), primary_h - NSMidY(on_screen)));
    CGAssociateMouseAndMouseCursorPosition(false);
    [NSCursor hide];
    g_cap.captured = true;
    std::fprintf(stderr, "[rt] host_win: mouse captured (hold Option or switch apps to release; Escape is only the pad's Menu)\n");
}

void reconcile_capture() {
    NSWindow* key = key_host_window();
    const bool wanted = key && NSApp.isActive && !key.isMiniaturized && gta6rt_gameinput_mouse_active();
    if (!key) g_cap.key_since = 0;
    if (g_cap.captured && (!wanted || g_cap.option_down)) {
        release_cursor();
        if (!wanted) gta6rt_gameinput_mouse_release();
    } else if (!g_cap.captured && wanted && g_cap.key_since > 0 && CFAbsoluteTimeGetCurrent() - g_cap.key_since >= 0.5) {
        capture_cursor(key);
    }
}

std::shared_ptr<Entry> find_entry(const gta6rt_mac_window* window) {
    std::lock_guard lock(g_mu);
    for (auto& e : g_entries)
        if (e->window == window) return e;
    return nullptr;
}

std::vector<std::shared_ptr<Entry>> all_entries() {
    std::lock_guard lock(g_mu);
    return g_entries;
}

bool is_host_window(NSWindow* ns) {
    if (!ns) return false;
    std::lock_guard lock(g_mu);
    for (auto& e : g_entries)
        if (e->ns == ns) return true;
    return false;
}

NSWindow* key_host_window() {
    NSWindow* key = NSApp.keyWindow;
    if (!key) return nil;
    std::lock_guard lock(g_mu);
    for (auto& e : g_entries)
        if (e->ns == key) return key;
    return nil;
}

void run_on_main(void (^block)(void)) {
    if ([NSThread isMainThread]) block();
    else dispatch_async(dispatch_get_main_queue(), block);
}

bool run_on_main_wait(void (^block)(void), int ms = 5000) {
    if ([NSThread isMainThread]) {
        block();
        return true;
    }
    auto done = std::make_shared<dispatch_semaphore_t>(dispatch_semaphore_create(0));
    dispatch_async(dispatch_get_main_queue(), ^{
        block();
        dispatch_semaphore_signal(*done);
    });
    return dispatch_semaphore_wait(*done, dispatch_time(DISPATCH_TIME_NOW, int64_t(ms) * NSEC_PER_MSEC)) == 0;
}

NSWindow* ns_window_of(gta6rt_mac_window* window) {
    CAMetalLayer* layer = (__bridge CAMetalLayer*)gta6rt_mac_window_metal_layer(window);
    if (!layer) return nil;
    for (NSWindow* w in NSApp.windows)
        if (w.contentView.layer == layer) return w;
    id delegate = layer.delegate;
    if ([delegate isKindOfClass:NSView.class]) return ((NSView*)delegate).window;
    return nil;
}

host_win_screen read_screen() {
    host_win_screen s{};
    NSScreen* screen = NSScreen.mainScreen ?: NSScreen.screens.firstObject;
    if (!screen) return s;
    s.scale = screen.backingScaleFactor;
    s.width_px = uint32_t(screen.frame.size.width * s.scale);
    s.height_px = uint32_t(screen.frame.size.height * s.scale);
    s.area_w_px = screen.visibleFrame.size.width * s.scale;
    s.area_h_px = screen.visibleFrame.size.height * s.scale;
    const NSRect frame = [NSWindow frameRectForContentRect:NSMakeRect(0, 0, 100, 100) styleMask:kHostStyle];
    s.chrome_h_px = (frame.size.height - 100) * s.scale;
    return s;
}

void emit(const std::shared_ptr<Entry>& e, host_win_event_kind kind, int32_t a = 0, int32_t b = 0, int32_t c = 0,
          int32_t d = 0, int32_t f = 0) {
    host_win_event ev{kind, {a, b, c, d, f, 0}};
    if (e->fn) e->fn(e->ctx, e->window, &ev);
}

void refresh_geometry(const std::shared_ptr<Entry>& e) {
    NSWindow* ns = e->ns;
    NSView* view = ns.contentView;
    const NSRect in_window = [view convertRect:view.bounds toView:nil];
    const NSRect on_screen = [ns convertRectToScreen:in_window];
    const CGFloat scale = ns.backingScaleFactor;
    const CGFloat primary_h = NSScreen.screens.firstObject.frame.size.height;
    e->x = int32_t(on_screen.origin.x * scale);
    e->y = int32_t((primary_h - on_screen.origin.y - on_screen.size.height) * scale);
    e->w = uint32_t(on_screen.size.width * scale);
    e->h = uint32_t(on_screen.size.height * scale);
}

// Full screen and zoom ignore contentAspectRatio, so the view (the layer the title's frames go to) is fitted to the
// title's aspect inside the window's usable area and the rest of the window stays black.
void letterbox(const std::shared_ptr<Entry>& e) {
    NSWindow* ns = e->ns;
    NSView* view = ns.contentView;
    if (!e->logical_w || !e->logical_h || !view) return;
    const NSRect area = ns.contentLayoutRect;
    if (area.size.width < 1 || area.size.height < 1) return;
    const double aspect = double(e->logical_w) / e->logical_h;
    double w = area.size.width, h = w / aspect;
    if (h > area.size.height) h = area.size.height, w = h * aspect;
    NSRect fit = NSMakeRect(area.origin.x + (area.size.width - w) / 2, area.origin.y + (area.size.height - h) / 2, w, h);
    fit = NSIntegralRect(fit);
    if (!NSEqualRects(view.frame, fit)) view.frame = fit;
}

int size_state(const std::shared_ptr<Entry>& e) { return e->minimized ? 1 : e->fullscreen ? 2 : 0; }

void emit_size(const std::shared_ptr<Entry>& e) {
    letterbox(e);
    refresh_geometry(e);
    emit(e, HOST_WIN_EV_SIZE, size_state(e), int32_t(e->w.load()), int32_t(e->h.load()));
}

void emit_display(const std::shared_ptr<Entry>& e) {
    NSScreen* screen = e->ns.screen ?: NSScreen.mainScreen;
    const CGFloat scale = screen.backingScaleFactor;
    emit(e, HOST_WIN_EV_DISPLAY, int32_t(screen.frame.size.width * scale), int32_t(screen.frame.size.height * scale), 32);
}

}  // namespace

@interface Gta6rtMenuTarget : NSObject
@end

@implementation Gta6rtMenuTarget
- (void)toggleFullScreen:(id)sender {
    (void)sender;
    NSWindow* key = NSApp.keyWindow;
    auto entries = all_entries();
    if (entries.empty()) return;
    for (auto& e : entries)
        if (e->ns == key) {
            [e->ns toggleFullScreen:nil];
            return;
        }
    [entries.front()->ns toggleFullScreen:nil];
}
- (void)quit:(id)sender {
    (void)sender;
    auto entries = all_entries();
    if (!entries.empty()) emit(entries.front(), HOST_WIN_EV_CLOSE);
}
@end

namespace {

Gta6rtMenuTarget* g_menu_target = nil;
id g_key_monitor = nil;
id g_mouse_monitor = nil;

NSMenuItem* make_item(NSString* title, SEL action, NSString* key, NSEventModifierFlags flags) {
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
    item.keyEquivalentModifierMask = flags;
    return item;
}

void install_app_once() {
    if (g_app_installed) return;
    g_app_installed = true;
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    [center addObserverForName:NSApplicationDidBecomeActiveNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification*) {
                        for (auto& e : all_entries()) emit(e, HOST_WIN_EV_APP_ACTIVE, 1);
                    }];
    [center addObserverForName:NSApplicationDidResignActiveNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification*) {
                        for (auto& e : all_entries()) emit(e, HOST_WIN_EV_APP_ACTIVE, 0);
                    }];
    [center addObserverForName:NSApplicationDidChangeScreenParametersNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification*) {
                        for (auto& e : all_entries()) emit_display(e);
                    }];

    g_menu_target = [Gta6rtMenuTarget new];
    NSMenuItem* full = make_item(@"Toggle Full Screen", @selector(toggleFullScreen:), @"f",
                                 NSEventModifierFlagControl | NSEventModifierFlagCommand);
    NSMenuItem* f11 = make_item(@"Toggle Full Screen (F11)", @selector(toggleFullScreen:),
                                [NSString stringWithFormat:@"%C", (unichar)NSF11FunctionKey], 0);
    full.target = g_menu_target;
    f11.target = g_menu_target;
    bool populated = false;
    for (NSMenuItem* item in NSApp.mainMenu.itemArray)
        if (item.submenu.itemArray.count > 0) populated = true;
    if (!populated) {
        NSMenu* bar = [NSMenu new];
        NSMenuItem* app_item = [NSMenuItem new];
        [bar addItem:app_item];
        NSMenu* menu = [[NSMenu alloc] initWithTitle:@"Grand Theft Auto VI"];
        NSMenuItem* mini = make_item(@"Minimize", @selector(performMiniaturize:), @"m", NSEventModifierFlagCommand);
        NSMenuItem* quit = make_item(@"Quit", @selector(quit:), @"q", NSEventModifierFlagCommand);
        quit.target = g_menu_target;
        for (NSMenuItem* item in @[ full, f11, mini, [NSMenuItem separatorItem], quit ]) [menu addItem:item];
        app_item.submenu = menu;
        NSApp.mainMenu = bar;
    } else {
        std::fprintf(stderr, "[rt] host_win: the application has its own main menu; only a View menu with Toggle Full Screen is added\n");
        NSMenuItem* view_item = [NSMenuItem new];
        NSMenu* view = [[NSMenu alloc] initWithTitle:@"View"];
        [view addItem:full];
        [view addItem:f11];
        view_item.submenu = view;
        [NSApp.mainMenu addItem:view_item];
    }

    g_key_monitor = [NSEvent
        addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp | NSEventMaskFlagsChanged
                                     handler:^NSEvent*(NSEvent* event) {
                                         if (event.type == NSEventTypeFlagsChanged) {
                                             const bool down = (event.modifierFlags & NSEventModifierFlagOption) != 0;
                                             if (down != g_cap.option_down) {
                                                 g_cap.option_down = down;
                                                 reconcile_capture();
                                             }
                                         }
                                         if (!is_host_window(event.window)) return event;
                                         if (event.type == NSEventTypeKeyDown &&
                                             gta6rt_hud_handle_key(event.keyCode, event.modifierFlags, event.isARepeat))
                                             return nil;
                                         if (event.type == NSEventTypeKeyDown && [NSApp.mainMenu performKeyEquivalent:event])
                                             return nil;
                                         if (event.modifierFlags & NSEventModifierFlagCommand) return event;
                                         return nil;
                                     }];

    const char* mouse_env = std::getenv("GTA6RT_MOUSE");
    g_cap.off = !(mouse_env && (!std::strcmp(mouse_env, "on") || !std::strcmp(mouse_env, "capture") || !std::strcmp(mouse_env, "1")));
    std::atexit([] { CGAssociateMouseAndMouseCursorPosition(true); });
    g_mouse_monitor = [NSEvent
        addLocalMonitorForEventsMatchingMask:NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged |
                                             NSEventMaskRightMouseDragged | NSEventMaskOtherMouseDragged |
                                             NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp |
                                             NSEventMaskRightMouseDown | NSEventMaskRightMouseUp
                                     handler:^NSEvent*(NSEvent* event) {
                                         NSWindow* ns = event.window;
                                         if (!is_host_window(ns)) return event;
                                         if (event.type == NSEventTypeLeftMouseDown && !ns.isKeyWindow) {
                                             if (@available(macOS 14.0, *)) [NSApp activate];
                                             [ns makeKeyAndOrderFront:nil];
                                         }
                                         if (!ns.isKeyWindow || g_cap.option_down) return event;
                                         switch (event.type) {
                                             case NSEventTypeLeftMouseDown: gta6rt_gameinput_mouse_buttons(1, (NSEvent.pressedMouseButtons >> 1) & 1); break;
                                             case NSEventTypeLeftMouseUp: gta6rt_gameinput_mouse_buttons(0, (NSEvent.pressedMouseButtons >> 1) & 1); break;
                                             case NSEventTypeRightMouseDown: gta6rt_gameinput_mouse_buttons(NSEvent.pressedMouseButtons & 1, 1); break;
                                             case NSEventTypeRightMouseUp: gta6rt_gameinput_mouse_buttons(NSEvent.pressedMouseButtons & 1, 0); break;
                                             default:
                                                 gta6rt_gameinput_mouse_motion(event.deltaX, -event.deltaY);
                                                 break;
                                         }
                                         return event;
                                     }];
}

void observe(const std::shared_ptr<Entry>& e, NSNotificationName name, void (^body)(const std::shared_ptr<Entry>&)) {
    std::weak_ptr<Entry> weak = e;
    id token = [NSNotificationCenter.defaultCenter addObserverForName:name
                                                               object:e->ns
                                                                queue:nil
                                                           usingBlock:^(NSNotification*) {
                                                               if (auto strong = weak.lock()) body(strong);
                                                           }];
    [e->tokens addObject:token];
}

}  // namespace

extern "C" {

int host_win_screen_info(host_win_screen* out) {
    if (!out) return 0;
    auto result = std::make_shared<host_win_screen>();
    const bool ok = run_on_main_wait(^{ *result = read_screen(); });
    *out = *result;
    return ok && out->scale > 0;
}

int host_win_plan_size(uint32_t want_w, uint32_t want_h, uint32_t* out_w, uint32_t* out_h) {
    host_win_screen s{};
    if (!host_win_screen_info(&s)) {
        *out_w = want_w;
        *out_h = want_h;
        return 0;
    }
    const host::win::Size fit = host::win::fit_client(want_w, want_h, s.area_w_px, s.area_h_px, s.chrome_h_px);
    *out_w = fit.w;
    *out_h = fit.h;
    return 1;
}

int host_win_attach(gta6rt_mac_window* window, uint32_t logical_w, uint32_t logical_h, host_win_event_fn fn, void* ctx) {
    auto result = std::make_shared<int>(0);
    const bool answered = run_on_main_wait(^{
        NSWindow* ns = ns_window_of(window);
        if (!ns) return;
        auto e = std::make_shared<Entry>();
        e->window = window;
        e->ns = ns;
        e->fn = fn;
        e->ctx = ctx;
        e->tokens = [NSMutableArray new];
        e->logical_w = logical_w;
        e->logical_h = logical_h;
        {
            std::lock_guard lock(g_mu);
            g_entries.push_back(e);
        }
        install_app_once();
        if (logical_w && logical_h) {
            ns.contentAspectRatio = NSMakeSize(logical_w, logical_h);
            ns.contentMinSize = NSMakeSize(320, 320.0 * logical_h / logical_w);
        }
        ns.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;
        ns.backgroundColor = NSColor.blackColor;
        if (@available(macOS 14.0, *)) [NSApp activate];
        letterbox(e);
        refresh_geometry(e);
        ns.acceptsMouseMovedEvents = YES;
        if (!g_cap.timer && !g_cap.off)
            g_cap.timer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer*) { reconcile_capture(); }];
        observe(e, NSWindowDidBecomeKeyNotification, (^(const std::shared_ptr<Entry>& x) {
                    emit(x, HOST_WIN_EV_FOCUS, 1);
                    g_cap.key_since = CFAbsoluteTimeGetCurrent();
                }));
        observe(e, NSWindowDidResignKeyNotification, (^(const std::shared_ptr<Entry>& x) {
                    emit(x, HOST_WIN_EV_FOCUS, 0);
                    g_cap.key_since = 0;
                    release_cursor();
                    gta6rt_gameinput_mouse_release();
                }));
        observe(e, NSWindowDidResizeNotification, (^(const std::shared_ptr<Entry>& x) { emit_size(x); }));
        observe(e, NSWindowDidMoveNotification, (^(const std::shared_ptr<Entry>& x) {
                    refresh_geometry(x);
                    emit(x, HOST_WIN_EV_MOVE, x->x.load(), x->y.load());
                }));
        observe(e, NSWindowDidMiniaturizeNotification, (^(const std::shared_ptr<Entry>& x) {
                    x->minimized = true;
                    emit_size(x);
                }));
        observe(e, NSWindowDidDeminiaturizeNotification, (^(const std::shared_ptr<Entry>& x) {
                    x->minimized = false;
                    emit_size(x);
                }));
        observe(e, NSWindowDidChangeScreenNotification, (^(const std::shared_ptr<Entry>& x) { emit_display(x); }));
        observe(e, NSWindowDidChangeBackingPropertiesNotification, (^(const std::shared_ptr<Entry>& x) {
                    letterbox(x);
                    refresh_geometry(x);
                    const int32_t dpi = int32_t(96.0 * x->ns.backingScaleFactor);
                    emit(x, HOST_WIN_EV_DPI, dpi, x->x.load(), x->y.load(), x->x.load() + int32_t(x->w.load()),
                         x->y.load() + int32_t(x->h.load()));
                }));
        observe(e, NSWindowDidEnterFullScreenNotification, (^(const std::shared_ptr<Entry>& x) {
                    x->fullscreen = true;
                    emit(x, HOST_WIN_EV_FULLSCREEN, 1);
                    emit_size(x);
                }));
        observe(e, NSWindowDidExitFullScreenNotification, (^(const std::shared_ptr<Entry>& x) {
                    x->fullscreen = false;
                    emit(x, HOST_WIN_EV_FULLSCREEN, 0);
                    emit_size(x);
                }));
        gta6rt_hud_attach((__bridge void*)ns);
        *result = 1;
    });
    if (!answered) std::fprintf(stderr, "[rt] host_win: the main queue did not answer attaching window %p\n", (void*)window);
    return *result;
}

void host_win_detach(gta6rt_mac_window* window) {
    auto e = find_entry(window);
    if (!e) return;
    {
        std::lock_guard lock(g_mu);
        g_entries.erase(std::remove(g_entries.begin(), g_entries.end(), e), g_entries.end());
    }
    run_on_main(^{
        release_cursor();
        gta6rt_gameinput_mouse_release();
        if (all_entries().empty() && g_cap.timer) {
            [g_cap.timer invalidate];
            g_cap.timer = nil;
        }
        gta6rt_hud_detach((__bridge void*)e->ns);
        for (id token in e->tokens) [NSNotificationCenter.defaultCenter removeObserver:token];
        [e->tokens removeAllObjects];
    });
}

int host_win_show(gta6rt_mac_window* window, int cmd) {
    auto e = find_entry(window);
    if (!e) return 0;
    if (cmd < 0 || cmd > 10) return 0;
    run_on_main(^{
        NSWindow* ns = e->ns;
        // isZoomed is also true for a window that merely fills the screen (the title's 3840x2160 does), so only a
        // zoom this layer made is undone.
        switch (cmd) {
            case 0: [ns orderOut:nil]; break;
            case 2:
            case 6:
            case 7: [ns miniaturize:nil]; break;
            case 3:
                if (!e->zoomed_by_us) {
                    e->zoomed_by_us = true;
                    [ns zoom:nil];
                }
                break;
            case 4:
            case 8:
                if (ns.isMiniaturized) [ns deminiaturize:nil];
                [ns orderFront:nil];
                break;
            default:
                if (ns.isMiniaturized) [ns deminiaturize:nil];
                if (e->zoomed_by_us && cmd == 9) {
                    e->zoomed_by_us = false;
                    [ns zoom:nil];
                }
                [ns makeKeyAndOrderFront:nil];
                break;
        }
    });
    return 1;
}

int host_win_set_fullscreen(gta6rt_mac_window* window, int on) {
    auto e = find_entry(window);
    if (!e) return 0;
    run_on_main(^{
        const bool is = (e->ns.styleMask & NSWindowStyleMaskFullScreen) != 0;
        if (is != (on != 0)) [e->ns toggleFullScreen:nil];
    });
    return 1;
}

int host_win_is_fullscreen(const gta6rt_mac_window* window) {
    auto e = find_entry(window);
    return e && e->fullscreen.load();
}

int host_win_client_rect(const gta6rt_mac_window* window, int32_t* x, int32_t* y, uint32_t* w, uint32_t* h) {
    auto e = find_entry(window);
    if (!e) return 0;
    if (x) *x = e->x.load();
    if (y) *y = e->y.load();
    if (w) *w = e->w.load();
    if (h) *h = e->h.load();
    return 1;
}

}  // extern "C"
