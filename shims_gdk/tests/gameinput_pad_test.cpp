// The pure mappings of gameinput_pad.h: a physical controller's elements and the keyboard pad onto GameInput's
// gamepad state. No device, no GameInput object.
#include <cmath>
#include <cstdio>

#include "gameinput_pad.h"

using namespace rt::gameinput_mac;

namespace {
int g_failed = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok) ++g_failed;
}
bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }
KeySet keys(std::initializer_list<int> list) {
    KeySet k;
    for (int c : list) k.set(static_cast<size_t>(c));
    return k;
}
}  // namespace

int main() {
    PadElements e;
    check(pad_state_from_elements(e) == PadState{}, "an idle controller is the zero state");

    e.a = e.right_shoulder = e.options = e.up = e.left_thumb = true;
    e.left_trigger = 0.5f;
    e.lx = -1.0f;
    e.ly = 0.25f;
    PadState s = pad_state_from_elements(e);
    check(s.buttons == (btn::A | btn::RightShoulder | btn::View | btn::Up | btn::LeftThumb),
          "A, right shoulder, options (View), d-pad up and left stick click map to GameInput bits");
    check(s.lt == 0.5f && s.lx == -1.0f && s.ly == 0.25f && s.rt == 0 && s.rx == 0 && s.ry == 0,
          "triggers are 0..1 and sticks keep GameController's +Y up");

    PadElements wild;
    wild.left_trigger = 3.0f;
    wild.lx = -9.0f;
    wild.ry = NAN;
    s = pad_state_from_elements(wild);
    check(s.lt == 1.0f && s.lx == -1.0f && s.ry == 0.0f, "out-of-range and non-finite values are clamped");

    PadElements menu;
    menu.menu = menu.b = menu.x = menu.y = menu.left_shoulder = menu.right_thumb = true;
    menu.down = menu.left = menu.right = true;
    check(pad_state_from_elements(menu).buttons ==
              (btn::Menu | btn::B | btn::X | btn::Y | btn::LeftShoulder | btn::RightThumb | btn::Down | btn::Left | btn::Right),
          "every other button has its own bit");

    check(pad_state_from_keys(KeySet{}) == PadState{}, "no keys is the zero state");
    s = pad_state_from_keys(keys({key::Return}));
    check(s.buttons == btn::A, "Return is A");
    check(pad_state_from_keys(keys({key::Space})).buttons == btn::A && pad_state_from_keys(keys({key::KeypadEnter})).buttons == btn::A,
          "Space and keypad Enter are A too");
    check(pad_state_from_keys(keys({key::Escape})).buttons == btn::Menu && pad_state_from_keys(keys({key::Tab})).buttons == btn::View,
          "Escape is Menu and Tab is View");
    check(pad_state_from_keys(keys({key::Backspace})).buttons == btn::B, "Backspace is B");
    check(pad_state_from_keys(keys({key::X, key::Y})).buttons == (btn::X | btn::Y), "X and Y are X and Y");
    check(pad_state_from_keys(keys({key::Q, key::E})).buttons == (btn::LeftShoulder | btn::RightShoulder),
          "Q and E are the shoulders");
    check(pad_state_from_keys(keys({key::Up, key::Left})).buttons == (btn::Up | btn::Left), "arrows are the d-pad");
    check(pad_state_from_keys(keys({key::F, key::G})).buttons == (btn::LeftThumb | btn::RightThumb), "F and G click the sticks");
    s = pad_state_from_keys(keys({key::Z, key::C}));
    check(s.lt == 1.0f && s.rt == 1.0f && s.buttons == 0, "Z and C are the triggers");

    s = pad_state_from_keys(keys({key::W}));
    check(s.lx == 0 && s.ly == 1.0f, "W pushes the left stick up to full");
    s = pad_state_from_keys(keys({key::A}));
    check(s.lx == -1.0f && s.ly == 0 && s.buttons == 0, "A key is stick left, not the A button");
    s = pad_state_from_keys(keys({key::W, key::S}));
    check(s.ly == 0, "opposite keys cancel");
    s = pad_state_from_keys(keys({key::W, key::D}));
    check(near(s.lx, 0.70710678f) && near(s.ly, 0.70710678f), "a diagonal sits on the round gate");
    s = pad_state_from_keys(keys({key::J, key::K}));
    check(near(s.rx, -0.70710678f) && near(s.ry, -0.70710678f), "I/J/K/L drive the right stick");
    s = pad_state_from_keys(keys({key::W, key::S, key::D}));
    check(s.lx == 1.0f && s.ly == 0, "with one axis cancelled the other stays full");

    float mx = 0, my = 0;
    for (int i = 0; i < 400; ++i) mouse_stick_step(&mx, &my, 0.0, 10.0, 0.005, 1.0);
    check(std::fabs(my - 1.0f) < 0.01f && mx == 0, "steady 2000 counts a second up is full right-stick up");
    for (int i = 0; i < 400; ++i) mouse_stick_step(&mx, &my, 0.0, 5.0, 0.005, 1.0);
    check(std::fabs(my - 0.5f) < 0.01f, "half the speed is half deflection");
    for (int i = 0; i < 400; ++i) mouse_stick_step(&mx, &my, 0.0, 40.0, 0.005, 1.0);
    check(my == 1.0f, "faster than full is clamped");
    int steps = 0;
    while ((mx != 0 || my != 0) && steps < 1000) mouse_stick_step(&mx, &my, 0, 0, 0.002, 1.0), ++steps;
    check(my == 0 && steps >= 40 && steps <= 80, "a stopped mouse falls back to exactly 0 in about 130 ms (2 ms steps)");
    mx = my = 0;
    for (int i = 0; i < 400; ++i) mouse_stick_step(&mx, &my, -6.0, -6.0, 0.005, 2.0);
    check(mx == -1.0f && my == -1.0f, "left and down are negative; sensitivity 2 doubles the gain");
    mx = my = 0;
    mouse_stick_step(&mx, &my, 100, 0, 0.0, 1.0);
    check(mx == 0, "a zero time step changes nothing");
    s = with_mouse(pad_state_from_keys(keys({key::J})), 0.25f, 0.5f, true, true);
    check(near(s.rx, -0.75f) && s.ry == 0.5f && s.rt == 1.0f && s.lt == 1.0f,
          "the mouse adds to the right-stick keys; left button is the right trigger, right button the left trigger");
    s = with_mouse(pad_state_from_keys(keys({key::L})), 0.9f, 0, false, false);
    check(s.rx == 1.0f && s.rt == 0 && s.lt == 0, "the sum is clamped to full");

    std::printf("%d failure(s)\n", g_failed);
    return g_failed ? 1 : 0;
}
