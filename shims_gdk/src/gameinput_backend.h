// The seam between the GameInput COM layer (gameinput_mac.cpp) and what feeds it. The GameController backend
// (gameinput_gc.mm) drives a Sink from its own threads; the unit tests drive the same Sink with synthetic state.
// No windows.h: this header is included from Objective-C++.
#pragma once
#include <cstdint>
#include <memory>

#include "gameinput_pad.h"

namespace rt::gameinput_mac {

class Sink {
public:
    virtual ~Sink() = default;
    // A pad (or the keyboard pad) is present. The returned handle names it in state() and disconnect(). The same
    // desc.key connecting again gets its device object back.
    virtual uint64_t connect(const PadDesc& desc) = 0;
    virtual void disconnect(uint64_t handle) = 0;
    virtual void state(uint64_t handle, const PadState& state, bool guide) = 0;
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual void start(Sink& sink) = 0;
    virtual void set_rumble(uint64_t handle, const RumbleLevels& levels) = 0;
    virtual bool battery(uint64_t handle, BatteryInfo* out) = 0;
};

// The hub's Sink, for tests and for backends created before the first GameInputCreate.
Sink& sink();

// Installs the backend the hub starts instead of the GameController one (GTA6RT_INPUT=test leaves none, for tests
// that only use sink()). Call before the first GameInputCreate.
void set_backend(std::unique_ptr<Backend> backend);

// Observes SetRumbleState calls that reached the hub (after clamping and GTA6RT_RUMBLE), whatever the backend.
using RumbleObserver = void (*)(uint64_t handle, const RumbleLevels& levels);
void set_rumble_observer(RumbleObserver observer);

// The clock readings and the scripted pad's timeline use, in microseconds (GetCurrentTimestamp's scale). Set it before
// the first GameInputCreate; nullptr restores the performance counter.
using ClockFn = uint64_t (*)();
void set_clock(ClockFn now_us);

// Runs the calling thread's run loop for `seconds`: GameController posts connect and disconnect notifications on the
// main thread's run loop, so a tool or test whose main thread would otherwise block calls this.
void pump_run_loop(double seconds);

// The mouse as the keyboard pad's right stick (relative movement, +dy_up when the mouse moves up) and triggers (left
// button = right trigger, right button = left trigger). Fed by the host's window layer from the key window's events.
// Ignored while the keyboard pad is not connected or GTA6RT_MOUSE=off. mouse_release zeroes everything (focus lost).
void mouse_motion(double dx, double dy_up);
void mouse_buttons(bool left, bool right);
void mouse_release();
// True while mouse input would reach the title: the keyboard pad is connected and GTA6RT_MOUSE is not off.
bool mouse_pad_active();
// Defined by the hub: the keyboard pad is connected now.
bool keyboard_pad_connected();

// The GameController backend; null where the framework is unavailable.
std::unique_ptr<Backend> make_gc_backend();

}  // namespace rt::gameinput_mac
