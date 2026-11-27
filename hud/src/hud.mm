#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <mach/mach.h>
#include <sys/sysctl.h>

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "gta6rt_hud/hud.h"
#include "hud_model.h"

using gta6rt::hud::Colour;
using gta6rt::hud::FormatOptions;
using gta6rt::hud::Line;
using gta6rt::hud::Metrics;
using gta6rt::hud::Raw;
using gta6rt::hud::Sampler;

@interface Gta6rtHudPanel : NSPanel
@end

@implementation Gta6rtHudPanel
- (BOOL)canBecomeKeyWindow {
    return NO;
}
- (BOOL)canBecomeMainWindow {
    return NO;
}
- (BOOL)acceptsFirstResponder {
    return NO;
}
- (NSTimeInterval)animationResizeTime:(NSRect)newFrame {
    return 0;
}
@end

namespace {

constexpr double kDefaultRate = 4.0;
constexpr double kDefaultBudgetMs = 33.4;
constexpr CGFloat kFontSize = 10.0;
constexpr CGFloat kPadX = 6.0, kPadY = 3.0, kMargin = 6.0;

std::atomic<int> g_mode{GTA6RT_HUD_OFF};
std::atomic<int> g_last_mode{GTA6RT_HUD_MIN};
std::atomic<double> g_rate{kDefaultRate};
std::atomic<double> g_budget{kDefaultBudgetMs};
std::atomic<double> g_log_every{0};
std::atomic<int> g_pressure{0};
std::atomic<int> g_disabled{0};
std::atomic<bool> g_atexit{false};
std::atomic<uint64_t (*)(void*)> g_import_fn{nullptr};
std::atomic<void*> g_import_ctx{nullptr};

std::atomic<uint64_t> c_ticks{0}, c_tick_ns{0}, c_tick_max{0}, c_title_sets{0}, c_title_ns{0}, c_title_max{0},
    c_overlay_updates{0}, c_interval_ns{0}, c_interval_max{0}, c_interval_n{0}, c_last_tick{0};
std::atomic<int> g_attached{0}, g_visible{0}, g_timer{0};
std::atomic<uint32_t> g_window_number{0};
std::mutex g_line_mutex;
char g_line[160];

struct Ui {
    __weak NSWindow* parent = nil;
    Gta6rtHudPanel* panel = nil;
    CATextLayer* text = nil;
    NSTimer* timer = nil;
    NSMutableArray* tokens = nil;
    dispatch_source_t pressure = nil;
    NSString* original_title = nil;
    bool title_applied = false;
    bool warming = false;
    int warm_ticks = 0;
    uint64_t warm_presents = 0;
    double timer_rate = 0;
    CGFloat width_mark = 0;
    int width_mode = -1;
    std::string last_signature;
    NSSize last_text_size = NSZeroSize;
    Sampler sampler;
    uint64_t imports_base = 0;
    double last_log_t = 0;
};

Ui* ui() {
    static Ui* instance = new Ui;
    return instance;
}

uint64_t now_ns() { return gta6rt_hud_now_ns(); }

void log_line(const char* text) { std::fprintf(stderr, "[rt] hud: %s\n", text); }

void raise_max(std::atomic<uint64_t>& a, uint64_t v) {
    uint64_t seen = a.load(std::memory_order_relaxed);
    while (v > seen && !a.compare_exchange_weak(seen, v, std::memory_order_relaxed)) {
    }
}

void on_main(void (^block)(void)) {
    if ([NSThread isMainThread]) block();
    else dispatch_async(dispatch_get_main_queue(), block);
}

uint64_t physical_ram() {
    static const uint64_t ram = [] {
        uint64_t v = 0;
        size_t size = sizeof v;
        if (sysctlbyname("hw.memsize", &v, &size, nullptr, 0) != 0) return uint64_t(0);
        return v;
    }();
    return ram;
}

void read_system(Raw& r) {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        r.footprint = info.phys_footprint;
        r.compressed = info.compressed;
        r.resident = info.resident_size;
    }
    r.ram = physical_ram();
    r.pressure = g_pressure.load(std::memory_order_relaxed);
}

Raw read_raw() {
    gta6rt_hud_stats* b = gta6rt_hud_stats_block();
    Raw r;
    r.t_s = double(now_ns()) * 1e-9;
    r.have = __atomic_load_n(&b->have, __ATOMIC_RELAXED);
    r.presents = __atomic_load_n(&b->presents, __ATOMIC_RELAXED);
    r.frame_intervals = __atomic_load_n(&b->frame_intervals, __ATOMIC_RELAXED);
    r.frame_ns_sum = __atomic_load_n(&b->frame_ns_sum, __ATOMIC_RELAXED);
    r.frame_ns_max = __atomic_exchange_n(&b->frame_ns_max, 0, __ATOMIC_RELAXED);
    r.draws = __atomic_load_n(&b->draws, __ATOMIC_RELAXED);
    r.dispatches = __atomic_load_n(&b->dispatches, __ATOMIC_RELAXED);
    r.submissions = __atomic_load_n(&b->submissions, __ATOMIC_RELAXED);
    r.exec_cpu_ns = __atomic_load_n(&b->exec_cpu_ns, __ATOMIC_RELAXED);
    r.gpu_ns = __atomic_load_n(&b->gpu_ns, __ATOMIC_RELAXED);
    r.gpu_samples = __atomic_load_n(&b->gpu_samples, __ATOMIC_RELAXED);
    r.latency_ns_sum = __atomic_load_n(&b->latency_ns_sum, __ATOMIC_RELAXED);
    r.latency_samples = __atomic_load_n(&b->latency_samples, __ATOMIC_RELAXED);
    r.latency_ns_max = __atomic_load_n(&b->latency_ns_max, __ATOMIC_RELAXED);
    r.pipelines_built = __atomic_load_n(&b->pipelines_built, __ATOMIC_RELAXED);
    r.pipelines_pending = __atomic_load_n(&b->pipelines_pending, __ATOMIC_RELAXED);
    r.cache_hits = __atomic_load_n(&b->pipeline_cache_hits, __ATOMIC_RELAXED);
    r.pso_failures = __atomic_load_n(&b->pso_failures, __ATOMIC_RELAXED);
    r.textures_waiting = __atomic_load_n(&b->textures_waiting, __ATOMIC_RELAXED);
    const uint64_t last_present = __atomic_load_n(&b->last_present_ns, __ATOMIC_RELAXED);
    const uint64_t now = now_ns();
    r.since_present_s = last_present && now > last_present ? double(now - last_present) * 1e-9 : 0.0;
    r.imports = __atomic_load_n(&b->guest_imports, __ATOMIC_RELAXED);
    r.imports_valid = (r.have & GTA6RT_HUD_HAVE_IMPORTS) != 0;
    if (auto fn = g_import_fn.load(std::memory_order_acquire)) {
        r.imports = fn(g_import_ctx.load(std::memory_order_relaxed));
        r.imports_valid = true;
    }
    char text[GTA6RT_HUD_STRING_LEN];
    if (gta6rt_hud_get_string(&b->state, text)) r.state = text;
    if (gta6rt_hud_get_string(&b->driver, text)) r.driver = text;
    if (gta6rt_hud_get_string(&b->profile, text)) r.profile = text;
    r.have_memory = (r.have & GTA6RT_HUD_HAVE_MEMORY) != 0;
    r.budget = __atomic_load_n(&b->mem_budget, __ATOMIC_RELAXED);
    r.gpu_bytes = __atomic_load_n(&b->gpu_bytes, __ATOMIC_RELAXED);
    r.gpu_limit = __atomic_load_n(&b->gpu_limit, __ATOMIC_RELAXED);
    r.governor_level = __atomic_load_n(&b->governor_level, __ATOMIC_RELAXED);
    r.mem_warning = __atomic_load_n(&b->mem_warning, __ATOMIC_RELAXED);
    if (gta6rt_hud_get_string(&b->mem_note, text)) r.mem_note = text;
    read_system(r);
    return r;
}

NSFont* hud_font() {
    NSFont* f = [NSFont monospacedSystemFontOfSize:kFontSize weight:NSFontWeightMedium];
    return f ?: [NSFont userFixedPitchFontOfSize:kFontSize];
}

NSColor* colour_for(Colour c) {
    switch (c) {
        case Colour::amber: return [NSColor colorWithSRGBRed:1.0 green:0.78 blue:0.2 alpha:1.0];
        case Colour::red: return [NSColor colorWithSRGBRed:1.0 green:0.35 blue:0.3 alpha:1.0];
        default: return [NSColor colorWithWhite:1.0 alpha:0.95];
    }
}

NSAttributedString* build_text(const std::vector<Line>& lines) {
    NSMutableParagraphStyle* style = [NSMutableParagraphStyle new];
    style.lineBreakMode = NSLineBreakByClipping;
    style.alignment = NSTextAlignmentLeft;
    NSDictionary* base = @{
        NSFontAttributeName : hud_font(),
        NSForegroundColorAttributeName : colour_for(Colour::normal),
        NSParagraphStyleAttributeName : style
    };
    NSMutableAttributedString* out = [NSMutableAttributedString new];
    NSUInteger offset = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string text = lines[i].text;
        if (i + 1 < lines.size()) text += "\n";
        NSString* s = [NSString stringWithUTF8String:text.c_str()] ?: @"";
        [out appendAttributedString:[[NSAttributedString alloc] initWithString:s attributes:base]];
        for (const auto& span : lines[i].spans)
            [out addAttribute:NSForegroundColorAttributeName
                        value:colour_for(span.colour)
                        range:NSMakeRange(offset + span.begin, span.length)];
        offset += s.length;
    }
    return out;
}

std::string signature_of(const std::vector<Line>& lines) {
    std::string s;
    for (const auto& l : lines) {
        s += l.text;
        s += '\n';
        for (const auto& sp : l.spans) s += std::to_string(sp.begin) + ":" + std::to_string(sp.length) + ":" + std::to_string(int(sp.colour)) + ";";
        s += '\n';
    }
    return s;
}

void place_panel(Ui* u) {
    NSWindow* parent = u->parent;
    if (!parent || !u->panel) return;
    const NSRect content = [parent convertRectToScreen:parent.contentLayoutRect];
    NSSize size = u->panel.frame.size;
    CGFloat maxw = std::max<CGFloat>(40.0, content.size.width - 2 * kMargin);
    if (size.width > maxw) size.width = maxw;
    NSRect frame = NSMakeRect(content.origin.x + kMargin, NSMaxY(content) - kMargin - size.height, size.width, size.height);
    if (!NSEqualRects(frame, u->panel.frame)) [u->panel setFrame:frame display:NO];
    const CGFloat scale = parent.backingScaleFactor;
    if (u->text.contentsScale != scale) u->text.contentsScale = scale;
    if (u->panel.level != parent.level) u->panel.level = parent.level;
    if (u->panel.parentWindow != parent) [parent addChildWindow:u->panel ordered:NSWindowAbove];
    g_visible.store(u->panel.isVisible ? 1 : 0);
    g_window_number.store(uint32_t(u->panel.windowNumber));
}

void create_panel(Ui* u) {
    NSWindow* parent = u->parent;
    if (!parent || u->panel) return;
    Gta6rtHudPanel* p = [[Gta6rtHudPanel alloc] initWithContentRect:NSMakeRect(0, 0, 200, 20)
                                                          styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                                                            backing:NSBackingStoreBuffered
                                                              defer:NO];
    p.opaque = NO;
    p.backgroundColor = NSColor.clearColor;
    p.hasShadow = NO;
    p.ignoresMouseEvents = YES;
    p.releasedWhenClosed = NO;
    p.hidesOnDeactivate = NO;
    p.floatingPanel = NO;
    p.becomesKeyOnlyIfNeeded = YES;
    p.worksWhenModal = NO;
    p.movable = NO;
    p.animationBehavior = NSWindowAnimationBehaviorNone;
    p.level = parent.level;
    p.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorIgnoresCycle |
                           NSWindowCollectionBehaviorTransient;
    p.title = @"";
    NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 200, 20)];
    view.wantsLayer = YES;
    view.layer.backgroundColor = [NSColor colorWithWhite:0.0 alpha:0.42].CGColor;
    view.layer.cornerRadius = 4.0;
    view.layer.masksToBounds = YES;
    CATextLayer* t = [CATextLayer layer];
    t.alignmentMode = kCAAlignmentLeft;
    t.truncationMode = kCATruncationNone;
    t.wrapped = NO;
    t.contentsScale = parent.backingScaleFactor;
    t.allowsFontSubpixelQuantization = YES;
    [view.layer addSublayer:t];
    p.contentView = view;
    u->panel = p;
    u->text = t;
    u->last_signature.clear();
    u->width_mark = 0;
    [parent addChildWindow:p ordered:NSWindowAbove];
    place_panel(u);
}

void destroy_panel(Ui* u) {
    if (!u->panel) {
        g_visible.store(0);
        g_window_number.store(0);
        return;
    }
    NSWindow* parent = u->panel.parentWindow;
    if (parent) [parent removeChildWindow:u->panel];
    [u->panel orderOut:nil];
    [u->panel close];
    u->panel = nil;
    u->text = nil;
    u->last_signature.clear();
    g_visible.store(0);
    g_window_number.store(0);
}

void restore_title(Ui* u) {
    if (u->title_applied) {
        NSWindow* parent = u->parent;
        if (parent && u->original_title) parent.title = u->original_title;
    }
    u->title_applied = false;
    u->original_title = nil;
}

void update_panel(Ui* u, const std::vector<Line>& lines, int mode) {
    if (!u->panel) return;
    const std::string sig = signature_of(lines);
    if (sig == u->last_signature && mode == u->width_mode) return;
    u->last_signature = sig;
    NSAttributedString* attr = build_text(lines);
    NSRect bounds = [attr boundingRectWithSize:NSMakeSize(100000, 100000) options:NSStringDrawingUsesLineFragmentOrigin];
    CGFloat w = std::ceil(bounds.size.width) + 2 * kPadX;
    CGFloat h = std::ceil(bounds.size.height) + 2 * kPadY;
    if (mode != u->width_mode) {
        u->width_mark = 0;
        u->width_mode = mode;
    }
    if (w > u->width_mark) u->width_mark = w;
    w = u->width_mark;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    u->text.string = attr;
    u->text.frame = CGRectMake(kPadX, kPadY, w - 2 * kPadX, h - 2 * kPadY);
    [CATransaction commit];
    NSRect f = u->panel.frame;
    if (std::fabs(f.size.width - w) > 0.5 || std::fabs(f.size.height - h) > 0.5) {
        f.size = NSMakeSize(w, h);
        [u->panel setFrame:f display:NO];
    }
    place_panel(u);
    c_overlay_updates.fetch_add(1, std::memory_order_relaxed);
}

void stop_timer(Ui* u) {
    if (u->timer) {
        [u->timer invalidate];
        u->timer = nil;
    }
    u->timer_rate = 0;
    g_timer.store(0);
}

void stop_pressure(Ui* u) {
    if (u->pressure) {
        dispatch_source_cancel(u->pressure);
        u->pressure = nil;
    }
    g_pressure.store(0);
}

void remove_tokens(Ui* u) {
    for (id token in u->tokens) [NSNotificationCenter.defaultCenter removeObserver:token];
    [u->tokens removeAllObjects];
}

void tick();
void teardown_window(Ui* u);

void start_pressure(Ui* u) {
    if (u->pressure) return;
    dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
                                                   DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL |
                                                       DISPATCH_MEMORYPRESSURE_NORMAL,
                                                   dispatch_get_main_queue());
    if (!src) return;
    dispatch_source_set_event_handler(src, ^{
        const unsigned long d = dispatch_source_get_data(src);
        if (d & DISPATCH_MEMORYPRESSURE_CRITICAL) g_pressure.store(2);
        else if (d & DISPATCH_MEMORYPRESSURE_WARN) g_pressure.store(1);
        else if (d & DISPATCH_MEMORYPRESSURE_NORMAL) g_pressure.store(0);
    });
    dispatch_resume(src);
    u->pressure = src;
}

void start_timer(Ui* u) {
    const double rate = g_rate.load();
    if (u->timer && u->timer_rate == rate) return;
    stop_timer(u);
    NSTimer* t = [NSTimer timerWithTimeInterval:1.0 / rate repeats:YES block:^(NSTimer*) { tick(); }];
    t.tolerance = 0.002;
    [[NSRunLoop mainRunLoop] addTimer:t forMode:NSRunLoopCommonModes];
    u->timer = t;
    u->timer_rate = rate;
    g_timer.store(1);
}

void observe_parent(Ui* u) {
    NSWindow* parent = u->parent;
    remove_tokens(u);
    if (!parent) return;
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    NSArray* names = @[
        NSWindowDidResizeNotification, NSWindowDidMoveNotification, NSWindowDidChangeScreenNotification,
        NSWindowDidChangeBackingPropertiesNotification, NSWindowDidEnterFullScreenNotification,
        NSWindowDidExitFullScreenNotification, NSWindowDidDeminiaturizeNotification, NSWindowDidExposeNotification
    ];
    for (NSNotificationName name in names) {
        id token = [center addObserverForName:name
                                       object:parent
                                        queue:nil
                                   usingBlock:^(NSNotification*) {
                                       Ui* v = ui();
                                       if (v->panel) place_panel(v);
                                   }];
        [u->tokens addObject:token];
    }
    id closing = [center addObserverForName:NSWindowWillCloseNotification
                                     object:parent
                                      queue:nil
                                 usingBlock:^(NSNotification*) { teardown_window(ui()); }];
    [u->tokens addObject:closing];
}

void teardown_runtime(Ui* u) {
    stop_timer(u);
    destroy_panel(u);
    restore_title(u);
    stop_pressure(u);
    remove_tokens(u);
}

void teardown_window(Ui* u) {
    teardown_runtime(u);
    u->parent = nil;
    g_attached.store(0);
}

void disable(const char* why) {
    if (g_disabled.exchange(1) == 0) {
        char buf[200];
        std::snprintf(buf, sizeof buf, "overlay disabled: %s", why);
        log_line(buf);
    }
    gta6rt_hud_set_active(0);
    Ui* u = ui();
    teardown_runtime(u);
}

void apply() {
    Ui* u = ui();
    @try {
        const int mode = g_mode.load();
        if (mode == GTA6RT_HUD_OFF || g_disabled.load()) {
            teardown_runtime(u);
            return;
        }
        NSWindow* parent = u->parent;
        if (!parent) {
            stop_timer(u);
            destroy_panel(u);
            return;
        }
        u->sampler.reset();
        if (!u->timer) {
            u->warming = true;
            u->warm_ticks = 0;
            u->warm_presents = __atomic_load_n(&gta6rt_hud_stats_block()->presents, __ATOMIC_RELAXED);
        }
        if (u->tokens.count == 0) observe_parent(u);
        start_pressure(u);
        if (mode == GTA6RT_HUD_TITLE) {
            destroy_panel(u);
            if (!u->title_applied) {
                u->original_title = parent.title;
                u->title_applied = true;
            }
        } else {
            restore_title(u);
            create_panel(u);
        }
        start_timer(u);
        tick();
    } @catch (NSException* e) {
        disable(e.reason.UTF8String ? e.reason.UTF8String : "exception");
    }
}

void log_metrics(const char* tag, const Metrics& m, const Raw& raw, FormatOptions opt) {
    opt.mode = GTA6RT_HUD_FULL;
    const std::vector<Line> full = gta6rt::hud::format_lines(m, opt);
    std::string joined;
    for (const Line& l : full) joined += (joined.empty() ? "" : " || ") + l.text;
    std::fprintf(stderr,
                 "[rt] hud%s: %s || raw presents %llu draws %llu dispatches %llu submissions %llu pso built %llu pending %llu failed %llu\n",
                 tag, joined.c_str(), static_cast<unsigned long long>(raw.presents), static_cast<unsigned long long>(raw.draws),
                 static_cast<unsigned long long>(raw.dispatches), static_cast<unsigned long long>(raw.submissions),
                 static_cast<unsigned long long>(raw.pipelines_built), static_cast<unsigned long long>(raw.pipelines_pending),
                 static_cast<unsigned long long>(raw.pso_failures));
}

dispatch_source_t g_headless = nil;

void headless_tick() {
    static Sampler sampler;
    static double last_log = 0;
    static bool primed = false;
    if (g_attached.load() || g_disabled.load() || g_mode.load() == GTA6RT_HUD_OFF) {
        primed = false;
        return;
    }
    const Raw raw = read_raw();
    if (!primed) {
        sampler.reset();
        sampler.update(raw);
        last_log = raw.t_s;
        primed = true;
        return;
    }
    const Metrics m = sampler.update(raw);
    if (raw.t_s - last_log < g_log_every.load()) return;
    last_log = raw.t_s;
    FormatOptions opt;
    opt.budget_ms = g_budget.load();
    log_metrics(" (no window)", m, raw, opt);
}

void start_headless() {
    if (g_headless || !(g_log_every.load() > 0)) return;
    dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    if (!src) return;
    const uint64_t interval = uint64_t(1e9 / g_rate.load());
    dispatch_source_set_timer(src, dispatch_time(DISPATCH_TIME_NOW, int64_t(interval)), interval, 10 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(src, ^{ headless_tick(); });
    dispatch_resume(src);
    g_headless = src;
}

void tick() {
    Ui* u = ui();
    const uint64_t t0 = now_ns();
    @try {
        NSWindow* parent = u->parent;
        const int mode = g_mode.load();
        if (mode == GTA6RT_HUD_OFF) return;
        if (!parent) {
            teardown_window(u);
            return;
        }
        Raw raw = read_raw();
        Metrics m;
        if (u->warming) {
            if (raw.presents != u->warm_presents || ++u->warm_ticks >= 3) {
                u->warming = false;
                u->sampler.reset();
                u->sampler.update(raw);
            }
        } else {
            m = u->sampler.update(raw);
        }
        FormatOptions opt;
        opt.mode = static_cast<gta6rt_hud_mode>(mode);
        opt.budget_ms = g_budget.load();
        const double log_every = g_log_every.load();
        if (!u->warming && log_every > 0 && raw.t_s - u->last_log_t >= log_every) {
            u->last_log_t = raw.t_s;
            log_metrics("", m, raw, opt);
        }
        if (mode == GTA6RT_HUD_TITLE) {
            const std::string base = u->original_title ? std::string(u->original_title.UTF8String ?: "") : std::string();
            const std::string title = gta6rt::hud::format_title(base, m, opt);
            NSString* s = [NSString stringWithUTF8String:title.c_str()];
            if (s) {
                const uint64_t a = now_ns();
                parent.title = s;
                const uint64_t d = now_ns() - a;
                c_title_sets.fetch_add(1, std::memory_order_relaxed);
                c_title_ns.fetch_add(d, std::memory_order_relaxed);
                raise_max(c_title_max, d);
            }
            std::lock_guard lock(g_line_mutex);
            std::snprintf(g_line, sizeof g_line, "%s", title.c_str());
        } else {
            std::vector<Line> lines = gta6rt::hud::format_lines(m, opt);
            update_panel(u, lines, mode);
            std::lock_guard lock(g_line_mutex);
            std::snprintf(g_line, sizeof g_line, "%s", lines.empty() ? "" : lines[0].text.c_str());
        }
    } @catch (NSException* e) {
        disable(e.reason.UTF8String ? e.reason.UTF8String : "exception");
    }
    const uint64_t d = now_ns() - t0;
    const uint64_t last = c_last_tick.exchange(t0, std::memory_order_relaxed);
    if (last && t0 > last && t0 - last < 5000000000ull) {
        c_interval_ns.fetch_add(t0 - last, std::memory_order_relaxed);
        c_interval_n.fetch_add(1, std::memory_order_relaxed);
        raise_max(c_interval_max, t0 - last);
    }
    c_ticks.fetch_add(1, std::memory_order_relaxed);
    c_tick_ns.fetch_add(d, std::memory_order_relaxed);
    raise_max(c_tick_max, d);
}

void atexit_teardown() {
    g_mode.store(GTA6RT_HUD_OFF);
    gta6rt_hud_set_active(0);
    if ([NSThread isMainThread]) teardown_runtime(ui());
}

void ensure_atexit() {
    if (!g_atexit.exchange(true)) std::atexit(atexit_teardown);
}

double clamp_rate(double hz) {
    if (!(hz > 0) || !std::isfinite(hz)) return kDefaultRate;
    return hz < 0.5 ? 0.5 : hz > 30.0 ? 30.0 : hz;
}

double env_double(const char* name, double fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    char* end = nullptr;
    const double d = std::strtod(v, &end);
    if (end == v || *end || !(d > 0)) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "%s=%s is not a positive number; ignored", name, v);
        log_line(buf);
        return fallback;
    }
    return d;
}

void set_mode_internal(gta6rt_hud_mode mode) {
    g_mode.store(mode);
    if (mode != GTA6RT_HUD_OFF) g_last_mode.store(mode);
    gta6rt_hud_set_active(mode != GTA6RT_HUD_OFF && !g_disabled.load());
    on_main(^{ apply(); });
}

}

extern "C" {

int gta6rt_hud_parse_mode(const char* text, gta6rt_hud_mode* out) {
    if (!text || !out) return 0;
    std::string s(text);
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (s == "off" || s == "0" || s == "none" || s.empty()) *out = GTA6RT_HUD_OFF;
    else if (s == "min" || s == "minimal" || s == "on" || s == "1") *out = GTA6RT_HUD_MIN;
    else if (s == "full" || s == "2") *out = GTA6RT_HUD_FULL;
    else if (s == "title") *out = GTA6RT_HUD_TITLE;
    else return 0;
    return 1;
}

const char* gta6rt_hud_mode_name(gta6rt_hud_mode mode) {
    switch (mode) {
        case GTA6RT_HUD_OFF: return "off";
        case GTA6RT_HUD_MIN: return "min";
        case GTA6RT_HUD_FULL: return "full";
        case GTA6RT_HUD_TITLE: return "title";
    }
    return "off";
}

void gta6rt_hud_init(const gta6rt_hud_config* config) {
    ensure_atexit();
    g_rate.store(clamp_rate(config && config->rate_hz > 0 ? config->rate_hz : env_double("GTA6RT_HUD_RATE", kDefaultRate)));
    g_budget.store(config && config->budget_ms > 0 ? config->budget_ms : env_double("GTA6RT_HUD_BUDGET_MS", kDefaultBudgetMs));
    g_disabled.store(0);
    g_log_every.store(env_double("GTA6RT_HUD_LOG", 0));
    set_mode_internal(config ? config->mode : GTA6RT_HUD_OFF);
    start_headless();
}

void gta6rt_hud_init_from_env(void) {
    gta6rt_hud_config config{};
    const char* v = std::getenv("GTA6RT_HUD");
    if (v && *v) {
        if (!gta6rt_hud_parse_mode(v, &config.mode)) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "GTA6RT_HUD=%s is not off|min|full|title; overlay stays off", v);
            log_line(buf);
            config.mode = GTA6RT_HUD_OFF;
        }
    }
    gta6rt_hud_init(&config);
}

int gta6rt_hud_attach(void* ns_window) {
    if (!ns_window) {
        if (g_mode.load() != GTA6RT_HUD_OFF) disable("no window to attach to (layer-only or hidden window)");
        return 0;
    }
    NSWindow* w = (__bridge NSWindow*)ns_window;
    on_main(^{
        Ui* u = ui();
        if (![w isKindOfClass:NSWindow.class]) {
            disable("not an NSWindow");
            return;
        }
        if (u->parent == w) return;
        if (u->parent || u->tokens.count || u->panel || u->timer) teardown_window(u);
        if (!u->tokens) u->tokens = [NSMutableArray new];
        u->parent = w;
        g_disabled.store(0);
        g_attached.store(1);
        gta6rt_hud_set_active(g_mode.load() != GTA6RT_HUD_OFF);
        apply();
    });
    return 1;
}

void gta6rt_hud_detach(void* ns_window) {
    NSWindow* w = (__bridge NSWindow*)ns_window;
    on_main(^{
        Ui* u = ui();
        if (u->parent == w || !u->parent) teardown_window(u);
    });
}

void gta6rt_hud_set_mode(gta6rt_hud_mode mode) {
    if (mode < GTA6RT_HUD_OFF || mode > GTA6RT_HUD_TITLE) return;
    set_mode_internal(mode);
}

gta6rt_hud_mode gta6rt_hud_get_mode(void) { return static_cast<gta6rt_hud_mode>(g_mode.load()); }

void gta6rt_hud_set_rate(double hz) {
    g_rate.store(clamp_rate(hz));
    on_main(^{ apply(); });
}

void gta6rt_hud_set_import_counter(uint64_t (*fn)(void*), void* ctx) {
    g_import_ctx.store(ctx);
    g_import_fn.store(fn, std::memory_order_release);
}

int gta6rt_hud_handle_key(unsigned short keycode, unsigned long flags, int is_repeat) {
    if (keycode != GTA6RT_HUD_KEY_F1 && keycode != GTA6RT_HUD_KEY_F2) return 0;
    const unsigned long blocking = NSEventModifierFlagShift | NSEventModifierFlagControl | NSEventModifierFlagOption |
                                   NSEventModifierFlagCommand;
    if (flags & blocking) return 0;
    if (is_repeat) return 1;
    const int mode = g_mode.load();
    if (keycode == GTA6RT_HUD_KEY_F1) {
        set_mode_internal(static_cast<gta6rt_hud_mode>((mode + 1) % 4));
    } else if (mode != GTA6RT_HUD_OFF) {
        set_mode_internal(GTA6RT_HUD_OFF);
    } else {
        set_mode_internal(static_cast<gta6rt_hud_mode>(g_last_mode.load()));
    }
    return 1;
}

void gta6rt_hud_get_info(gta6rt_hud_info* out) {
    if (!out) return;
    std::memset(out, 0, sizeof *out);
    out->mode = static_cast<gta6rt_hud_mode>(g_mode.load());
    out->attached = g_attached.load();
    out->overlay_visible = g_visible.load();
    out->timer_running = g_timer.load();
    out->disabled = g_disabled.load();
    out->rate_hz = g_rate.load();
    out->ticks = c_ticks.load();
    out->tick_ns_total = c_tick_ns.load();
    out->tick_ns_max = c_tick_max.load();
    out->title_sets = c_title_sets.load();
    out->title_set_ns_total = c_title_ns.load();
    out->title_set_ns_max = c_title_max.load();
    out->overlay_updates = c_overlay_updates.load();
    out->tick_interval_ns_total = c_interval_ns.load();
    out->tick_interval_ns_max = c_interval_max.load();
    out->tick_interval_count = c_interval_n.load();
    out->memory_pressure = g_pressure.load();
    out->overlay_window_number = g_window_number.load();
    std::lock_guard lock(g_line_mutex);
    std::snprintf(out->line, sizeof out->line, "%s", g_line);
}

void gta6rt_hud_shutdown(void) {
    g_mode.store(GTA6RT_HUD_OFF);
    gta6rt_hud_set_active(0);
    on_main(^{
        Ui* u = ui();
        teardown_window(u);
    });
}

void gta6rt_hud_test_set_pressure(int level) { g_pressure.store(level); }

void gta6rt_hud_test_reset_counters(void) {
    c_ticks = 0;
    c_tick_ns = 0;
    c_tick_max = 0;
    c_title_sets = 0;
    c_title_ns = 0;
    c_title_max = 0;
    c_overlay_updates = 0;
    c_interval_ns = 0;
    c_interval_max = 0;
    c_interval_n = 0;
    c_last_tick = 0;
}

}
