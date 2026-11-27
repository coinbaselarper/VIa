// LEONIDA_INPUT_SCRIPT, shared by both GameInput shims: the SDL3 one (gameinput.cpp, where the script drives the
// keyboard pad) and the macOS host's (gameinput_mac.cpp, where it drives a scripted pad of its own).
// The parser, the press timeline, the button names and the press log lines live here, once; nothing in this file
// needs SDL. docs/gameinput.md, "Scripted input", has the format.
//
//   LEONIDA_INPUT_SCRIPT="a@70,a@85,start@120,lsup@130:4,rsleft@136:2,key_w@140:3"
//
// Each entry is name@seconds[:hold], the seconds counted from the shim's first update. A press is held for its hold
// (kHoldSeconds when none is given), then released. Buttons: a b x y start back lb rb up down left right. Sticks:
// lsup lsdown lsleft lsright rsup rsdown rsleft rsright throw a stick fully that way; lt and rt pull a trigger fully.
// key_<k> holds a keyboard key and goes through the keyboard pad's mapping (gameinput_pad.h): k is a letter, return,
// space, escape (esc), backspace, tab or an arrow (arrowup arrowdown arrowleft arrowright). Spaces around an entry are
// allowed; empty entries, unknown names and invalid times or holds (negative, non-finite, trailing text, a hold of 0
// or over 600 s) are counted as rejected.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "gdk_env.h"

#include "gameinput_abi.h"
#include "gameinput_pad.h"

namespace rt::gameinput::script {

constexpr double kHoldSeconds = 0.3;

// One scriptable button: its name in the script and the press log, its GameInput gamepad bit, and its SDL3
// SDL_GamepadButton number, which the "scripted press of button <n>" line has always carried (tools/stage_timeline.py
// reads it). gameinput.cpp checks the numbers against SDL's enum.
struct Button {
    const char* name;
    uint32_t bit;
    int sdl;
};
inline constexpr Button kButtons[] = {
    {"a", gi::GameInputGamepadA, 0},              {"b", gi::GameInputGamepadB, 1},
    {"x", gi::GameInputGamepadX, 2},              {"y", gi::GameInputGamepadY, 3},
    {"start", gi::GameInputGamepadMenu, 6},       {"back", gi::GameInputGamepadView, 4},
    {"lb", gi::GameInputGamepadLeftShoulder, 9},  {"rb", gi::GameInputGamepadRightShoulder, 10},
    {"up", gi::GameInputGamepadDPadUp, 11},       {"down", gi::GameInputGamepadDPadDown, 12},
    {"left", gi::GameInputGamepadDPadLeft, 13},   {"right", gi::GameInputGamepadDPadRight, 14},
};

enum Axis : int { kNoAxis = -1, kLx = 0, kLy, kRx, kRy, kLt, kRt, kAxes };

struct Stick {
    const char* name;
    int axis;
    float value;
};
inline constexpr Stick kSticks[] = {
    {"lsup", kLy, 1},    {"lsdown", kLy, -1}, {"lsleft", kLx, -1}, {"lsright", kLx, 1},
    {"rsup", kRy, 1},    {"rsdown", kRy, -1}, {"rsleft", kRx, -1}, {"rsright", kRx, 1},
    {"lt", kLt, 1},      {"rt", kRt, 1},
};

struct Key {
    const char* name;
    int code;  // HID keyboard usage, gameinput_mac::key
};
inline constexpr Key kNamedKeys[] = {
    {"return", rt::gameinput_mac::key::Return},       {"enter", rt::gameinput_mac::key::Return},
    {"space", rt::gameinput_mac::key::Space},         {"escape", rt::gameinput_mac::key::Escape},
    {"esc", rt::gameinput_mac::key::Escape},          {"backspace", rt::gameinput_mac::key::Backspace},
    {"tab", rt::gameinput_mac::key::Tab},             {"arrowup", rt::gameinput_mac::key::Up},
    {"arrowdown", rt::gameinput_mac::key::Down},      {"arrowleft", rt::gameinput_mac::key::Left},
    {"arrowright", rt::gameinput_mac::key::Right},
};

constexpr double kLongestHold = 600.0;

struct Press {
    uint32_t button = 0;     // a GameInput gamepad bit, or 0
    int axis = kNoAxis;      // a stick or trigger, or kNoAxis
    float value = 0;         // the axis value while held
    int key = 0;             // a HID keyboard usage, or 0
    double at = 0;           // seconds
    double hold = kHoldSeconds;
    std::string name;
};

struct Script {
    std::vector<Press> presses;
    size_t rejected = 0;
    uint32_t named = 0;  // every button the script presses at least once
    bool empty() const { return presses.empty(); }
};

inline int key_code(const std::string& k) {
    if (k.size() == 1 && k[0] >= 'a' && k[0] <= 'z') return rt::gameinput_mac::key::A + (k[0] - 'a');
    for (const Key& n : kNamedKeys)
        if (k == n.name) return n.code;
    return 0;
}

inline bool parse_seconds(const char* text, const char** tail, double* out) {
    char* end;
    const double v = std::strtod(text, &end);
    if (end == text || !std::isfinite(v) || v < 0) return false;
    *tail = end;
    *out = v;
    return true;
}

// The script in `text`, matched without regard to case.
inline Script parse(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    Script out;
    if (text.empty()) return out;
    constexpr char whitespace[] = " \t\r\n\f\v";
    size_t items = 0;
    for (size_t at = 0, end; at <= text.size(); at = end + 1) {
        ++items;
        end = std::min(text.find(',', at), text.size());
        const std::string item = text.substr(at, end - at);
        const size_t first = item.find_first_not_of(whitespace);
        const size_t sep = item.find('@');
        if (sep == std::string::npos || first >= sep) continue;
        Press p;
        const char* tail;
        if (!parse_seconds(item.c_str() + sep + 1, &tail, &p.at)) continue;
        if (*tail == ':') {
            if (!parse_seconds(tail + 1, &tail, &p.hold) || p.hold <= 0 || p.hold > kLongestHold) continue;
        }
        if (item.find_first_not_of(whitespace, static_cast<size_t>(tail - item.c_str())) != std::string::npos) continue;
        p.name = item.substr(first, sep - first);
        bool known = false;
        for (const Button& b : kButtons) {
            if (p.name == b.name) {
                p.button = b.bit;
                out.named |= b.bit;
                known = true;
                break;
            }
        }
        for (const Stick& st : kSticks) {
            if (!known && p.name == st.name) {
                p.axis = st.axis;
                p.value = st.value;
                known = true;
            }
        }
        if (!known && p.name.rfind("key_", 0) == 0) {
            p.key = key_code(p.name.substr(4));
            known = p.key != 0;
        }
        if (known) out.presses.push_back(std::move(p));
    }
    out.rejected = items - out.presses.size();
    return out;
}

// What the script holds `seconds` after its clock started: the buttons (GameInput bits), the sticks and triggers, and
// the keyboard keys. One decision per button: a later entry for the same button must not release what an earlier
// entry holds (it did: with several entries only the last one ever reached the title).
struct Held {
    uint32_t buttons = 0;
    float axes[kAxes] = {};
    rt::gameinput_mac::KeySet keys;
    bool any() const {
        if (buttons || keys.any()) return true;
        for (float v : axes)
            if (v != 0) return true;
        return false;
    }
};
inline void hold(Held& h, const Press& p) {
    h.buttons |= p.button;
    if (p.axis != kNoAxis) h.axes[p.axis] = p.value;
    if (p.key) h.keys.set(static_cast<size_t>(p.key));
}
inline bool holds_at(const Press& p, double seconds) { return seconds >= p.at && seconds < p.at + p.hold; }
inline Held held_at(const Script& script, double seconds) {
    Held h;
    for (const Press& p : script.presses)
        if (holds_at(p, seconds)) hold(h, p);
    return h;
}

// The pad state a held set reads as: the keys through the keyboard pad's mapping, then the script's own buttons,
// sticks and triggers on top.
inline rt::gameinput_mac::PadState pad_of(const Held& h) {
    rt::gameinput_mac::PadState s = rt::gameinput_mac::pad_state_from_keys(h.keys);
    s.buttons |= h.buttons;
    const auto put = [&](float* v, int axis, float lo) {
        if (h.axes[axis] != 0) *v = rt::gameinput_mac::clamp_unit(h.axes[axis], lo, 1);
    };
    put(&s.lx, kLx, -1);
    put(&s.ly, kLy, -1);
    put(&s.rx, kRx, -1);
    put(&s.ry, kRy, -1);
    put(&s.lt, kLt, 0);
    put(&s.rt, kRt, 0);
    return s;
}

inline const Button* button_of(uint32_t bit) {
    for (const Button& b : kButtons)
        if (b.bit == bit) return &b;
    return nullptr;
}

// "[rt] gameinput: scripted press of button <n> at <t> s" for every button the script starts holding: `was` is the
// previous call's held set and is updated.
inline void log_scripted(uint32_t held, uint32_t* was, double seconds) {
    for (const Button& b : kButtons)
        if ((held & b.bit) && !(*was & b.bit))
            std::fprintf(stderr, "[rt] gameinput: scripted press of button %d at %.1f s\n", b.sdl, seconds);
    *was = held;
}

// The press log (LEONIDA_INPUT_LOG, on unless 0): every newly pressed button and every new right-stick throw in the
// script's own form, name@seconds, so a session played by hand can be replayed by joining the items with commas.
inline int stick_throw(float y) { return y > 0.6f ? 1 : y < -0.6f ? -1 : 0; }
inline void log_presses(uint32_t pressed, float right_y_was, float right_y, double seconds, const char* device) {
    for (const Button& b : kButtons)
        if (pressed & b.bit) std::fprintf(stderr, "[rt] gameinput: press %s@%.1f (%s)\n", b.name, seconds, device);
    const int was = stick_throw(right_y_was), is = stick_throw(right_y);
    if (is != was && is != 0)
        std::fprintf(stderr, "[rt] gameinput: press %s@%.1f (%s)\n", is > 0 ? "rsup" : "rsdown", seconds, device);
}

// UI navigation derived from a gamepad state, the same for every pad (VERIFY against the console's own mapping).
inline uint32_t ui_navigation(const gi::GameInputGamepadState& g) {
    using namespace gi;
    const uint32_t b = g.buttons;
    const float t = 0.5f;
    uint32_t u = 0;
    if (b & GameInputGamepadMenu) u |= GameInputUiNavigationMenu;
    if (b & GameInputGamepadView) u |= GameInputUiNavigationView;
    if (b & GameInputGamepadA) u |= GameInputUiNavigationAccept;
    if (b & GameInputGamepadB) u |= GameInputUiNavigationCancel;
    if (b & GameInputGamepadX) u |= GameInputUiNavigationContext1;
    if (b & GameInputGamepadY) u |= GameInputUiNavigationContext2;
    if (b & GameInputGamepadLeftThumbstick) u |= GameInputUiNavigationContext3;
    if (b & GameInputGamepadRightThumbstick) u |= GameInputUiNavigationContext4;
    if ((b & GameInputGamepadDPadUp) || g.leftThumbstickY > t) u |= GameInputUiNavigationUp;
    if ((b & GameInputGamepadDPadDown) || g.leftThumbstickY < -t) u |= GameInputUiNavigationDown;
    if ((b & GameInputGamepadDPadLeft) || g.leftThumbstickX < -t) u |= GameInputUiNavigationLeft;
    if ((b & GameInputGamepadDPadRight) || g.leftThumbstickX > t) u |= GameInputUiNavigationRight;
    if (b & GameInputGamepadLeftShoulder) u |= GameInputUiNavigationPageLeft;
    if (b & GameInputGamepadRightShoulder) u |= GameInputUiNavigationPageRight;
    if (g.leftTrigger > t) u |= GameInputUiNavigationPageUp;
    if (g.rightTrigger > t) u |= GameInputUiNavigationPageDown;
    if (g.rightThumbstickY > t) u |= GameInputUiNavigationScrollUp;
    if (g.rightThumbstickY < -t) u |= GameInputUiNavigationScrollDown;
    if (g.rightThumbstickX < -t) u |= GameInputUiNavigationScrollLeft;
    if (g.rightThumbstickX > t) u |= GameInputUiNavigationScrollRight;
    return u;
}

// An environment variable in lower case, of any length (a script is easily longer than a fixed buffer, and a value
// that did not fit used to read as unset). Empty when unset.
inline std::string env_lower(const char* name) {
    std::string s = rt::gdk::env_or(name);
    if (s.empty()) return {};
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace rt::gameinput::script
