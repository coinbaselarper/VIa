// GameInput's host input on Apple's GameController framework: gamepads (GCController, extended profile), the keyboard
// as a gamepad (GCKeyboard), and rumble through CoreHaptics. System frameworks only, so it works for x86_64 under
// Rosetta. The process must have AppKit loaded for GameController to deliver keyboards (measured: without it
// coalescedKeyboard stays nil; the host links Cocoa through d3d12_vulkan). No windows.h here. Controller and keyboard events arrive on a private serial queue; connect and
// disconnect notifications arrive on the thread that posts them (AppKit's main thread, so the host's main thread has
// to run its run loop, which it does).
#include "gameinput_backend.h"

#import <CoreHaptics/CoreHaptics.h>
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

@interface Gta6rtMotor : NSObject
- (instancetype)initWithEngine:(CHHapticEngine*)engine name:(NSString*)name;
- (void)setLevel:(float)level;
@end

@implementation Gta6rtMotor {
    CHHapticEngine* _engine;
    id<CHHapticAdvancedPatternPlayer> _player;
    NSString* _name;
    float _level;
    CFAbsoluteTime _started;
    BOOL _engineRunning;
}

- (instancetype)initWithEngine:(CHHapticEngine*)engine name:(NSString*)name {
    if ((self = [super init])) {
        _engine = engine;
        _name = name;
        __weak Gta6rtMotor* weak = self;
        _engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) {
            (void)reason;
            Gta6rtMotor* strong = weak;
            if (strong) strong->_engineRunning = NO, strong->_player = nil;
        };
        _engine.resetHandler = ^{
            Gta6rtMotor* strong = weak;
            if (strong) strong->_engineRunning = NO, strong->_player = nil;
        };
    }
    return self;
}

- (BOOL)ensureEngine {
    if (_engineRunning) return YES;
    NSError* error = nil;
    if (![_engine startAndReturnError:&error]) {
        std::fprintf(stderr, "[rt] gameinput: haptic engine %s did not start: %s\n", _name.UTF8String,
                     error.localizedDescription.UTF8String);
        return NO;
    }
    _engineRunning = YES;
    return YES;
}

- (void)stopPlayer {
    if (!_player) return;
    NSError* error = nil;
    [_player stopAtTime:0 error:&error];
    _player = nil;
}

- (BOOL)startPlayer:(float)level {
    if (![self ensureEngine]) return NO;
    NSError* error = nil;
    CHHapticEventParameter* intensity =
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1.0f];
    CHHapticEventParameter* sharpness =
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.5f];
    CHHapticEvent* event = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                                                         parameters:@[ intensity, sharpness ]
                                                       relativeTime:0
                                                           duration:30.0];
    CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[ event ] parameters:@[] error:&error];
    if (!pattern) return NO;
    _player = [_engine createAdvancedPlayerWithPattern:pattern error:&error];
    if (!_player || ![_player startAtTime:0 error:&error]) {
        _player = nil;
        return NO;
    }
    _started = CFAbsoluteTimeGetCurrent();
    CHHapticDynamicParameter* gain =
        [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl
                                                        value:level
                                                 relativeTime:0];
    [_player sendParameters:@[ gain ] atTime:0 error:&error];
    return YES;
}

- (void)setLevel:(float)level {
    if (level <= 0.001f) {
        if (_level > 0) [self stopPlayer];
        _level = 0;
        return;
    }
    if (!_player || CFAbsoluteTimeGetCurrent() - _started > 25.0) {
        [self stopPlayer];
        if (![self startPlayer:level]) {
            _level = 0;
            return;
        }
    } else if (level != _level) {
        NSError* error = nil;
        CHHapticDynamicParameter* gain =
            [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl
                                                            value:level
                                                     relativeTime:0];
        [_player sendParameters:@[ gain ] atTime:0 error:&error];
    }
    _level = level;
}
@end

namespace {

namespace mac = rt::gameinput_mac;

constexpr char kKeyboardName[] = "Keyboard pad (gta6rt)";

// GCKeyCode values are runtime constants; the keyboard map is written in HID usages, which they are documented to be.
bool key_codes_are_hid_usages() {
    using namespace rt::gameinput_mac;
    return GCKeyCodeKeyA == key::A && GCKeyCodeKeyW == key::W && GCKeyCodeKeyZ == key::Z &&
           GCKeyCodeReturnOrEnter == key::Return && GCKeyCodeEscape == key::Escape && GCKeyCodeSpacebar == key::Space &&
           GCKeyCodeUpArrow == key::Up && GCKeyCodeRightArrow == key::Right && GCKeyCodeKeypadEnter == key::KeypadEnter &&
           GCKeyCodeDeleteOrBackspace == key::Backspace && GCKeyCodeTab == key::Tab;
}

struct Attached {
    GCController* controller = nil;
    uint64_t handle = 0;
    std::string base_key;
    int twin = 0;
    Gta6rtMotor* low = nil;
    Gta6rtMotor* high = nil;
    Gta6rtMotor* left_trigger = nil;
    Gta6rtMotor* right_trigger = nil;
    bool combined_handles = false;
};

void vendor_product_ids(NSString* category, NSString* vendor, uint16_t* vid, uint16_t* pid) {
    NSString* s = [[NSString stringWithFormat:@"%@ %@", vendor ?: @"", category ?: @""] lowercaseString];
    *vid = 0;
    *pid = 0;
    if ([s containsString:@"xbox"]) *vid = 0x045E, *pid = 0x0B12;
    else if ([s containsString:@"dualsense"]) *vid = 0x054C, *pid = 0x0CE6;
    else if ([s containsString:@"dualshock"]) *vid = 0x054C, *pid = 0x09CC;
    else if ([s containsString:@"switch"] || [s containsString:@"nintendo"]) *vid = 0x057E, *pid = 0x2009;
}

class GcBackend;
std::atomic<GcBackend*> g_backend{nullptr};

class GcBackend final : public mac::Backend {
public:
    void start(mac::Sink& sink) override {
        sink_ = &sink;
        configure_mouse();
        g_backend.store(this);
        queue_ = dispatch_queue_create("gta6rt.gameinput", DISPATCH_QUEUE_SERIAL);
        rumble_queue_ = dispatch_queue_create("gta6rt.gameinput.rumble", DISPATCH_QUEUE_SERIAL);
        const char* background = std::getenv("GTA6RT_INPUT_BACKGROUND");
        if (background && *background && background[0] != '0') GCController.shouldMonitorBackgroundEvents = YES;
        NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
        [center addObserverForName:GCControllerDidConnectNotification
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification* n) { attach_controller((GCController*)n.object); }];
        [center addObserverForName:GCControllerDidDisconnectNotification
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification* n) { detach_controller((GCController*)n.object); }];
        [center addObserverForName:GCKeyboardDidConnectNotification
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification*) { attach_keyboard(); }];
        [center addObserverForName:GCKeyboardDidDisconnectNotification
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification*) { detach_keyboard(); }];
        for (GCController* c in GCController.controllers) attach_controller(c);
        keyboard_ok_ = key_codes_are_hid_usages();
        if (!keyboard_ok_)
            std::fprintf(stderr, "[rt] gameinput: GCKeyCode values are not HID usages on this system; keyboard pad disabled\n");
        attach_keyboard();
        std::fprintf(stderr, "[rt] gameinput: GameController backend started (%zu controller(s), keyboard %s)\n",
                     attached_count(), keyboard_handle_ ? "present" : "absent");
    }

    void set_rumble(uint64_t handle, const mac::RumbleLevels& levels) override {
        Gta6rtMotor *low = nil, *high = nil, *lt = nil, *rt = nil;
        bool combined = false;
        {
            std::lock_guard lock(mu_);
            for (const Attached& a : attached_)
                if (a.handle == handle) {
                    low = a.low, high = a.high, lt = a.left_trigger, rt = a.right_trigger, combined = a.combined_handles;
                }
        }
        if (!low && !high && !lt && !rt) return;
        const mac::RumbleLevels copy = levels;
        dispatch_async(rumble_queue_, ^{
            if (combined) [low setLevel:std::max(copy.low, copy.high)];
            else {
                [low setLevel:copy.low];
                [high setLevel:copy.high];
            }
            [lt setLevel:copy.left_trigger];
            [rt setLevel:copy.right_trigger];
        });
    }

    bool battery(uint64_t handle, mac::BatteryInfo* out) override {
        GCController* controller = nil;
        {
            std::lock_guard lock(mu_);
            for (const Attached& a : attached_)
                if (a.handle == handle) controller = a.controller;
        }
        GCDeviceBattery* battery = controller.battery;
        if (!battery) return false;
        out->level = battery.batteryLevel;
        switch (battery.batteryState) {
            case GCDeviceBatteryStateDischarging: out->state = 1; break;
            case GCDeviceBatteryStateCharging: out->state = 3; break;
            case GCDeviceBatteryStateFull: out->state = 2; break;
            default: out->state = -1; break;
        }
        return true;
    }

private:
    size_t attached_count() {
        std::lock_guard lock(mu_);
        return attached_.size();
    }

    static void read_gamepad(GCExtendedGamepad* gp, mac::PadState* state, bool* guide) {
        mac::PadElements e;
        e.a = gp.buttonA.pressed;
        e.b = gp.buttonB.pressed;
        e.x = gp.buttonX.pressed;
        e.y = gp.buttonY.pressed;
        e.left_shoulder = gp.leftShoulder.pressed;
        e.right_shoulder = gp.rightShoulder.pressed;
        e.left_thumb = gp.leftThumbstickButton.pressed;
        e.right_thumb = gp.rightThumbstickButton.pressed;
        e.up = gp.dpad.up.pressed;
        e.down = gp.dpad.down.pressed;
        e.left = gp.dpad.left.pressed;
        e.right = gp.dpad.right.pressed;
        e.menu = gp.buttonMenu.pressed;
        e.options = gp.buttonOptions.pressed;
        e.left_trigger = gp.leftTrigger.value;
        e.right_trigger = gp.rightTrigger.value;
        e.lx = gp.leftThumbstick.xAxis.value;
        e.ly = gp.leftThumbstick.yAxis.value;
        e.rx = gp.rightThumbstick.xAxis.value;
        e.ry = gp.rightThumbstick.yAxis.value;
        *state = mac::pad_state_from_elements(e);
        *guide = gp.buttonHome.pressed;
    }

    Gta6rtMotor* make_motor(GCController* controller, GCHapticsLocality locality) {
        CHHapticEngine* engine = [controller.haptics createEngineWithLocality:locality];
        if (!engine) {
            std::fprintf(stderr, "[rt] gameinput: no haptic engine for %s on \"%s\"\n", locality.UTF8String,
                         controller.vendorName.UTF8String);
            return nil;
        }
        return [[Gta6rtMotor alloc] initWithEngine:engine name:locality];
    }

    void attach_controller(GCController* controller) {
        if (!controller) return;
        GCExtendedGamepad* pad = controller.extendedGamepad;
        if (!pad) {
            std::fprintf(stderr, "[rt] gameinput: controller \"%s\" has no extended gamepad profile; ignored\n",
                         controller.vendorName.UTF8String);
            return;
        }
        Attached a;
        a.controller = controller;
        NSString* vendor = controller.vendorName ?: @"Controller";
        NSString* category = controller.productCategory ?: @"";
        {
            std::lock_guard lock(mu_);
            for (const Attached& other : attached_)
                if (other.controller == controller) return;
            a.base_key = std::string("gc:") + vendor.UTF8String + "/" + category.UTF8String;
            for (;; ++a.twin) {
                bool taken = false;
                for (const Attached& other : attached_) taken |= other.base_key == a.base_key && other.twin == a.twin;
                if (!taken) break;
            }
        }
        mac::PadDesc desc;
        desc.key = a.base_key + "#" + std::to_string(a.twin);
        desc.name = vendor.UTF8String;
        vendor_product_ids(category, vendor, &desc.vendor, &desc.product);

        if (!std::getenv("GTA6RT_RUMBLE") || std::getenv("GTA6RT_RUMBLE")[0] != '0') {
            NSSet<GCHapticsLocality>* supported = controller.haptics.supportedLocalities;
            if ([supported containsObject:GCHapticsLocalityLeftHandle] && [supported containsObject:GCHapticsLocalityRightHandle]) {
                a.low = make_motor(controller, GCHapticsLocalityLeftHandle);
                a.high = make_motor(controller, GCHapticsLocalityRightHandle);
            } else {
                for (GCHapticsLocality l in @[ GCHapticsLocalityHandles, GCHapticsLocalityAll, GCHapticsLocalityDefault ])
                    if (!a.low && [supported containsObject:l]) a.low = make_motor(controller, l), a.combined_handles = true;
            }
            if ([supported containsObject:GCHapticsLocalityLeftTrigger]) a.left_trigger = make_motor(controller, GCHapticsLocalityLeftTrigger);
            if ([supported containsObject:GCHapticsLocalityRightTrigger]) a.right_trigger = make_motor(controller, GCHapticsLocalityRightTrigger);
            if (a.low) desc.rumble_motors |= mac::kRumbleLow | (a.high || a.combined_handles ? mac::kRumbleHigh : 0);
            if (a.left_trigger) desc.rumble_motors |= mac::kRumbleLeftTrigger;
            if (a.right_trigger) desc.rumble_motors |= mac::kRumbleRightTrigger;
        }

        a.handle = sink_->connect(desc);
        {
            std::lock_guard lock(mu_);
            attached_.push_back(a);
        }
        controller.handlerQueue = queue_;
        const uint64_t handle = a.handle;
        mac::Sink* sink = sink_;
        pad.valueChangedHandler = ^(GCExtendedGamepad* gp, GCControllerElement*) {
            mac::PadState state;
            bool guide = false;
            read_gamepad(gp, &state, &guide);
            sink->state(handle, state, guide);
        };
        mac::PadState state;
        bool guide = false;
        read_gamepad(pad, &state, &guide);
        sink_->state(handle, state, guide);
        std::fprintf(stderr, "[rt] gameinput: controller \"%s\" (%s) attached, rumble motors %#x\n", vendor.UTF8String,
                     category.UTF8String, desc.rumble_motors);
    }

    void detach_controller(GCController* controller) {
        uint64_t handle = 0;
        {
            std::lock_guard lock(mu_);
            auto it = std::find_if(attached_.begin(), attached_.end(),
                                   [&](const Attached& a) { return a.controller == controller; });
            if (it == attached_.end()) return;
            handle = it->handle;
            attached_.erase(it);
        }
        controller.extendedGamepad.valueChangedHandler = nil;
        sink_->disconnect(handle);
    }

    void push_keyboard() {
        mac::KeySet keys;
        uint64_t handle;
        float mx, my;
        bool left, right;
        {
            std::lock_guard lock(mu_);
            keys = keys_;
            handle = keyboard_handle_;
            mx = mouse_x_, my = mouse_y_, left = mouse_left_, right = mouse_right_;
        }
        if (handle) sink_->state(handle, mac::with_mouse(mac::pad_state_from_keys(keys), mx, my, left, right), false);
    }

    void mouse_tick() {
        const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        bool idle;
        {
            std::lock_guard lock(mu_);
            const double dt = mouse_last_ns_ ? double(now - mouse_last_ns_) * 1e-9 : kMouseTickSeconds;
            mouse_last_ns_ = now;
            mac::mouse_stick_step(&mouse_x_, &mouse_y_, mouse_dx_, mouse_dy_, dt, mouse_sensitivity_);
            mouse_dx_ = mouse_dy_ = 0;
            idle = mouse_x_ == 0 && mouse_y_ == 0;
            if (idle && mouse_timer_) {
                dispatch_source_cancel(mouse_timer_);
                mouse_timer_ = nil;
                mouse_last_ns_ = 0;
            }
        }
        push_keyboard();
    }

    void ensure_mouse_timer() {
        if (mouse_timer_) return;
        dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue_);
        dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, int64_t(kMouseTickSeconds * 1e9)),
                                  uint64_t(kMouseTickSeconds * 1e9), 1000000);
        dispatch_source_set_event_handler(t, ^{ mouse_tick(); });
        mouse_timer_ = t;
        mouse_last_ns_ = 0;
        dispatch_resume(t);
    }

public:
    bool mouse_enabled() const { return mouse_mode_ != 0; }

    bool mouse_active() {
        std::lock_guard lock(mu_);
        return mouse_mode_ != 0 && keyboard_handle_ != 0;
    }

    void mouse_motion(double dx, double dy_up) {
        if (!(dx != 0 || dy_up != 0)) return;
        std::lock_guard lock(mu_);
        if (mouse_mode_ == 0 || !keyboard_handle_) return;
        mouse_dx_ += dx;
        mouse_dy_ += dy_up;
        ensure_mouse_timer();
    }

    void mouse_buttons(bool left, bool right) {
        {
            std::lock_guard lock(mu_);
            if (mouse_mode_ == 0 || !keyboard_handle_ || (mouse_left_ == left && mouse_right_ == right)) return;
            mouse_left_ = left;
            mouse_right_ = right;
        }
        push_keyboard();
    }

    void mouse_release() {
        {
            std::lock_guard lock(mu_);
            mouse_x_ = mouse_y_ = 0;
            mouse_dx_ = mouse_dy_ = 0;
            mouse_left_ = mouse_right_ = false;
            if (mouse_timer_) dispatch_source_cancel(mouse_timer_);
            mouse_timer_ = nil;
            mouse_last_ns_ = 0;
        }
        push_keyboard();
    }

private:

    void attach_keyboard() {
        if (!keyboard_ok_) return;
        GCKeyboard* keyboard = GCKeyboard.coalescedKeyboard;
        GCKeyboardInput* input = keyboard.keyboardInput;
        if (!input) return;
        {
            std::lock_guard lock(mu_);
            if (keyboard_handle_) return;
        }
        mac::PadDesc desc;
        desc.key = "gc:keyboard";
        desc.name = kKeyboardName;
        desc.kind = mac::PadKind::Keyboard;
        mac::KeySet initial;
        for (int code : mac::kKeyboardPadKeys)
            if ([input buttonForKeyCode:(GCKeyCode)code].pressed) initial.set(static_cast<size_t>(code));
        const uint64_t handle = sink_->connect(desc);
        {
            std::lock_guard lock(mu_);
            keyboard_handle_ = handle;
            keys_ = initial;
        }
        keyboard.handlerQueue = queue_;
        input.keyChangedHandler = ^(GCKeyboardInput*, GCControllerButtonInput*, GCKeyCode code, BOOL pressed) {
            if (code < 0 || code >= 256) return;
            {
                std::lock_guard lock(mu_);
                keys_.set(static_cast<size_t>(code), pressed);
            }
            push_keyboard();
        };
        push_keyboard();
    }

    void detach_keyboard() {
        if (GCKeyboard.coalescedKeyboard) return;
        uint64_t handle;
        {
            std::lock_guard lock(mu_);
            handle = keyboard_handle_;
            keyboard_handle_ = 0;
            keys_.reset();
        }
        if (handle) sink_->disconnect(handle);
    }

    mac::Sink* sink_ = nullptr;
    dispatch_queue_t queue_ = nil;
    dispatch_queue_t rumble_queue_ = nil;
    std::mutex mu_;
    std::vector<Attached> attached_;
    uint64_t keyboard_handle_ = 0;
    bool keyboard_ok_ = true;
    mac::KeySet keys_;
    static constexpr double kMouseTickSeconds = 0.005;
    int mouse_mode_ = 1;
    double mouse_sensitivity_ = 1.0;
    float mouse_x_ = 0, mouse_y_ = 0;
    double mouse_dx_ = 0, mouse_dy_ = 0;
    bool mouse_left_ = false, mouse_right_ = false;
    dispatch_source_t mouse_timer_ = nil;
    uint64_t mouse_last_ns_ = 0;

public:
    void configure_mouse() {
        const char* mode = std::getenv("GTA6RT_MOUSE");
        if (!(mode && (!std::strcmp(mode, "on") || !std::strcmp(mode, "capture") || !std::strcmp(mode, "1")))) mouse_mode_ = 0;
        if (const char* sens = std::getenv("GTA6RT_MOUSE_SENSITIVITY")) {
            char* end = nullptr;
            const double v = std::strtod(sens, &end);
            if (end != sens && !*end && v > 0 && v < 100) mouse_sensitivity_ = v;
            else std::fprintf(stderr, "[rt] gameinput: GTA6RT_MOUSE_SENSITIVITY=%s is not a number in (0, 100); using 1\n", sens);
        }
    }
};


}  // namespace

namespace rt::gameinput_mac {
std::unique_ptr<Backend> make_gc_backend() { return std::make_unique<GcBackend>(); }
void mouse_motion(double dx, double dy_up) {
    if (GcBackend* b = g_backend.load()) b->mouse_motion(dx, dy_up);
}
void mouse_buttons(bool left, bool right) {
    if (GcBackend* b = g_backend.load()) b->mouse_buttons(left, right);
}
void mouse_release() {
    if (GcBackend* b = g_backend.load()) b->mouse_release();
}
bool mouse_pad_active() {
    GcBackend* b = g_backend.load();
    return b && b->mouse_active() && keyboard_pad_connected();
}
void pump_run_loop(double seconds) { CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false); }
}  // namespace rt::gameinput_mac

extern "C" {
void gta6rt_gameinput_mouse_motion(double dx, double dy_up) { rt::gameinput_mac::mouse_motion(dx, dy_up); }
void gta6rt_gameinput_mouse_buttons(int left, int right) { rt::gameinput_mac::mouse_buttons(left != 0, right != 0); }
void gta6rt_gameinput_mouse_release(void) { rt::gameinput_mac::mouse_release(); }
int gta6rt_gameinput_mouse_active(void) { return rt::gameinput_mac::mouse_pad_active() ? 1 : 0; }
}
