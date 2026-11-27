// Gamepad state as the host input backends deliver it, and the two pure mappings onto it: a physical controller's
// elements (GameController's scale) and the keyboard pad. No windows.h and no AppKit, so the Objective-C++ backend
// (gameinput_gc.mm) and the unit tests both include it. The button bits are GameInput's; gameinput_mac.cpp asserts
// they equal gi::GameInputGamepadButtons.
#pragma once
#include <bitset>
#include <cmath>
#include <cstdint>
#include <string>

namespace rt::gameinput_mac {

namespace btn {
constexpr uint32_t Menu = 0x1, View = 0x2, A = 0x4, B = 0x8, X = 0x10, Y = 0x20, Up = 0x40, Down = 0x80, Left = 0x100,
                   Right = 0x200, LeftShoulder = 0x400, RightShoulder = 0x800, LeftThumb = 0x1000, RightThumb = 0x2000;
}

struct PadState {
    uint32_t buttons = 0;
    float lt = 0, rt = 0, lx = 0, ly = 0, rx = 0, ry = 0;  // sticks -1..1 with +Y up, triggers 0..1
    bool operator==(const PadState& o) const {
        return buttons == o.buttons && lt == o.lt && rt == o.rt && lx == o.lx && ly == o.ly && rx == o.rx && ry == o.ry;
    }
    bool operator!=(const PadState& o) const { return !(*this == o); }
};

struct PadElements {
    bool a = false, b = false, x = false, y = false;
    bool left_shoulder = false, right_shoulder = false, left_thumb = false, right_thumb = false;
    bool up = false, down = false, left = false, right = false;
    bool menu = false, options = false;
    float left_trigger = 0, right_trigger = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0;
};

inline float clamp_unit(float v, float lo, float hi) { return std::isfinite(v) ? (v < lo ? lo : v > hi ? hi : v) : 0.0f; }

inline PadState pad_state_from_elements(const PadElements& e) {
    PadState s;
    const struct {
        bool on;
        uint32_t bit;
    } map[] = {{e.menu, btn::Menu},         {e.options, btn::View},         {e.a, btn::A},
               {e.b, btn::B},               {e.x, btn::X},                  {e.y, btn::Y},
               {e.up, btn::Up},             {e.down, btn::Down},            {e.left, btn::Left},
               {e.right, btn::Right},       {e.left_shoulder, btn::LeftShoulder},
               {e.right_shoulder, btn::RightShoulder}, {e.left_thumb, btn::LeftThumb}, {e.right_thumb, btn::RightThumb}};
    for (const auto& m : map)
        if (m.on) s.buttons |= m.bit;
    s.lt = clamp_unit(e.left_trigger, 0, 1);
    s.rt = clamp_unit(e.right_trigger, 0, 1);
    s.lx = clamp_unit(e.lx, -1, 1);
    s.ly = clamp_unit(e.ly, -1, 1);
    s.rx = clamp_unit(e.rx, -1, 1);
    s.ry = clamp_unit(e.ry, -1, 1);
    return s;
}

// HID keyboard usages (usage page 0x07), which are GameController's GCKeyCode values.
namespace key {
constexpr int A = 0x04, C = 0x06, D = 0x07, E = 0x08, F = 0x09, G = 0x0A, I = 0x0C, J = 0x0D, K = 0x0E, L = 0x0F,
              Q = 0x14, S = 0x16, W = 0x1A, X = 0x1B, Y = 0x1C, Z = 0x1D, Return = 0x28, Escape = 0x29,
              Backspace = 0x2A, Tab = 0x2B, Space = 0x2C, Right = 0x4F, Left = 0x50, Down = 0x51, Up = 0x52,
              KeypadEnter = 0x58;
}

using KeySet = std::bitset<256>;

constexpr int kKeyboardPadKeys[] = {key::A,      key::C,      key::D,      key::E,     key::F,     key::G,
                                    key::I,      key::J,      key::K,      key::L,     key::Q,     key::S,
                                    key::W,      key::X,      key::Y,      key::Z,     key::Return, key::Escape,
                                    key::Backspace, key::Tab, key::Space,  key::Right, key::Left,  key::Down,
                                    key::Up,     key::KeypadEnter};

// The keyboard pad: arrows d-pad; Return, Space or keypad Enter = A; Backspace = B; X, Y; Q/E shoulders; Escape =
// Menu; Tab = View; W/A/S/D left stick; I/J/K/L right stick; F/G stick clicks; Z/C left/right trigger. Opposite keys
// cancel and a diagonal sits on the round gate (0.7071 per axis).
inline void stick_from_keys(bool neg_x, bool pos_x, bool neg_y, bool pos_y, float* x, float* y) {
    float fx = (pos_x ? 1.0f : 0.0f) - (neg_x ? 1.0f : 0.0f);
    float fy = (pos_y ? 1.0f : 0.0f) - (neg_y ? 1.0f : 0.0f);
    if (fx != 0.0f && fy != 0.0f) fx *= 0.70710678f, fy *= 0.70710678f;
    *x = fx;
    *y = fy;
}

inline PadState pad_state_from_keys(const KeySet& k) {
    auto on = [&](int code) { return k.test(static_cast<size_t>(code)); };
    PadElements e;
    e.up = on(key::Up);
    e.down = on(key::Down);
    e.left = on(key::Left);
    e.right = on(key::Right);
    e.a = on(key::Return) || on(key::Space) || on(key::KeypadEnter);
    e.b = on(key::Backspace);
    e.x = on(key::X);
    e.y = on(key::Y);
    e.left_shoulder = on(key::Q);
    e.right_shoulder = on(key::E);
    e.menu = on(key::Escape);
    e.options = on(key::Tab);
    e.left_thumb = on(key::F);
    e.right_thumb = on(key::G);
    e.left_trigger = on(key::Z) ? 1.0f : 0.0f;
    e.right_trigger = on(key::C) ? 1.0f : 0.0f;
    stick_from_keys(on(key::A), on(key::D), on(key::S), on(key::W), &e.lx, &e.ly);
    stick_from_keys(on(key::J), on(key::L), on(key::K), on(key::I), &e.rx, &e.ry);
    return pad_state_from_elements(e);
}

// The mouse as the keyboard pad's right stick and triggers. The stick follows movement, not position: each step it
// keeps 0.875 per 2 ms of what it held and adds the counts moved, scaled so a steady 2000 counts a second (times the
// sensitivity) is full deflection and a stopped mouse falls back to 0 in about 130 ms (leonida's gameinput.cpp).
constexpr double kMouseFullCountsPerSecond = 2000.0;
constexpr double kMouseDecayPer2ms = 0.875;

inline void mouse_stick_step(float* x, float* y, double dx, double dy_up, double dt, double sensitivity) {
    if (!(dt > 0)) return;
    const double d = std::pow(kMouseDecayPer2ms, dt / 0.002);
    const double gain = (1.0 / kMouseFullCountsPerSecond) * (1.0 - d) / dt * sensitivity;
    auto step = [&](float* v, double counts) {
        double n = *v * d + counts * gain;
        n = n > 1.0 ? 1.0 : n < -1.0 ? -1.0 : n;
        *v = (counts == 0.0 && std::fabs(n) < 0.004) ? 0.0f : static_cast<float>(n);
    };
    step(x, dx);
    step(y, dy_up);
}

inline PadState with_mouse(PadState s, float stick_x, float stick_y, bool left_button, bool right_button) {
    s.rx = clamp_unit(s.rx + stick_x, -1, 1);
    s.ry = clamp_unit(s.ry + stick_y, -1, 1);
    if (left_button) s.rt = 1.0f;
    if (right_button) s.lt = 1.0f;
    return s;
}

// Acceleration on top of the sensitivity: counts moved in dt are scaled by 1 + accel * speed / 2000 counts/s, the
// speed term capped at 2 (so accel 0.5 doubles a fast flick and leaves slow aiming as it was). The host's deltas
// already carry the system's pointer acceleration, so the default is 0.
inline double mouse_accelerated(double counts, double dt, double accel) {
    if (!(accel > 0) || !(dt > 0)) return counts;
    double speed = std::fabs(counts) / dt / kMouseFullCountsPerSecond;
    if (speed > 2.0) speed = 2.0;
    return counts * (1.0 + accel * speed);
}

constexpr int kMouseToggleKey = 0x3A;  // F1 (HID usage); Escape stays the title's pause (the pad's Menu)

// Mouse look as the keyboard pad sees it: accumulated deltas, the decaying stick, buttons, and the F1 pause. While
// paused (or disabled) motion and buttons are dropped and the stick reads 0, and active() is false, which is what makes
// the host window give the cursor back (it captures only while gta6rt_gameinput_mouse_active() is true).
struct MouseLook {
    bool enabled = false;
    bool paused = false;
    double sensitivity = 1.0;
    double accel = 0.0;
    float x = 0, y = 0;
    double dx = 0, dy = 0;
    bool left = false, right = false;

    bool active() const { return enabled && !paused; }
    void clear() { x = y = 0, dx = dy = 0, left = right = false; }
    // A key edge from the keyboard; true when it was the toggle key (consumed).
    bool key(int code, bool pressed) {
        if (code != kMouseToggleKey) return false;
        if (pressed && enabled) {
            paused = !paused;
            clear();
        }
        return true;
    }
    void motion(double mx, double my_up) {
        if (!active()) return;
        dx += mx;
        dy += my_up;
    }
    bool buttons(bool l, bool r) {
        if (!active() || (left == l && right == r)) return false;
        left = l, right = r;
        return true;
    }
    void step(double dt) {
        if (!active()) return clear();
        mouse_stick_step(&x, &y, mouse_accelerated(dx, dt, accel), mouse_accelerated(dy, dt, accel), dt, sensitivity);
        dx = dy = 0;
    }
    bool idle() const { return x == 0 && y == 0 && dx == 0 && dy == 0; }
    PadState apply(PadState s) const { return active() ? with_mouse(s, x, y, left, right) : s; }
};

enum class PadKind { Controller, Keyboard };

constexpr uint32_t kRumbleLow = 1, kRumbleHigh = 2, kRumbleLeftTrigger = 4, kRumbleRightTrigger = 8;

struct PadDesc {
    std::string key;
    std::string name;
    PadKind kind = PadKind::Controller;
    uint16_t vendor = 0, product = 0;
    uint32_t rumble_motors = 0;
    bool wireless = false;
};

struct RumbleLevels {
    float low = 0, high = 0, left_trigger = 0, right_trigger = 0;
};

struct BatteryInfo {
    float level = 0;
    int state = -1;  // GameInputBatteryStatus
};

}  // namespace rt::gameinput_mac
