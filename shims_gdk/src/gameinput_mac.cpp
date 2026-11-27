// gameinput.dll for the macOS host: GameInputCreate and the v0 IGameInput family over host input backends.
//
// Derived from leonida's runtime/shims/gameinput_no_sdl.cpp (GPL-3.0-or-later): the stub object, the scripted pad, the
// device/reading/dispatcher classes and the callback bookkeeping are that file's, generalised from one pad to a device
// list. The SDL3 shim (gameinput.cpp) is not used; devices come from a mac::Backend (gameinput_gc.mm, Apple's
// GameController framework) through a mac::Sink.
//
// Which object GameInputCreate serves is chosen on its first call (GTA6RT_INPUT):
//   none   the identity-only object: QueryInterface/AddRef/Release, E_NOTIMPL for every other slot (logged once).
//   auto   (default) GameController pads, the keyboard pad and the mouse, plus the scripted pad when
//          LEONIDA_INPUT_SCRIPT is set: a person can take over mid-run.
//   gc     the same as auto.
//   script the scripted pad alone (deterministic unattended runs); needs LEONIDA_INPUT_SCRIPT, else it is auto.
//   test   no backend; the hub's Sink (gameinput_backend.h) is driven by the caller. The scripted pad as in auto.
// GTA6RT_KEYBOARD_PAD: auto (default; the keyboard pad is connected only while no controller is), on, off.
//
// Readings are taken when a backend reports a change (the scripted pad: when the title asks). Each device keeps the
// last kHistoryDepth readings for GetNextReading/GetPreviousReading. Callbacks that run guest code run on a thread of
// the shim that may enter the guest, or on the title's thread through IGameInputDispatcher::Dispatch.
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "gameinput_abi.h"
#include "gameinput_backend.h"
#include "gameinput_script.h"
#include "gdk_thread.h"
#include "shims.h"

using namespace gi;
namespace mac = rt::gameinput_mac;
namespace script = rt::gameinput::script;

static_assert(mac::btn::Menu == GameInputGamepadMenu && mac::btn::View == GameInputGamepadView &&
              mac::btn::A == GameInputGamepadA && mac::btn::B == GameInputGamepadB && mac::btn::X == GameInputGamepadX &&
              mac::btn::Y == GameInputGamepadY && mac::btn::Up == GameInputGamepadDPadUp &&
              mac::btn::Down == GameInputGamepadDPadDown && mac::btn::Left == GameInputGamepadDPadLeft &&
              mac::btn::Right == GameInputGamepadDPadRight && mac::btn::LeftShoulder == GameInputGamepadLeftShoulder &&
              mac::btn::RightShoulder == GameInputGamepadRightShoulder &&
              mac::btn::LeftThumb == GameInputGamepadLeftThumbstick &&
              mac::btn::RightThumb == GameInputGamepadRightThumbstick);

namespace {

// ---- GTA6RT_INPUT=none: the identity-only object ------------------------------------------------------------------

constexpr size_t kSlots = 64;  // comfortably covers the public v0 IGameInput vtable

struct GameInputStub {
    const void* const* vtable;
};

std::atomic<uint32_t> g_refs{1};

HRESULT __stdcall stub_qi(GameInputStub* self, const IID& riid, void** out);
ULONG __stdcall stub_add_ref(GameInputStub*);
ULONG __stdcall stub_release(GameInputStub*);

template <size_t Slot>
HRESULT __stdcall stub_method(GameInputStub*) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "[rt] gameinput: slot %zu is not served with GTA6RT_INPUT=none; E_NOTIMPL (logged once)\n",
                     Slot);
    return E_NOTIMPL;
}

template <size_t... Slot>
constexpr auto make_vtable(std::index_sequence<Slot...>) {
    return std::array<void*, sizeof...(Slot)>{
        (Slot == 0   ? reinterpret_cast<void*>(&stub_qi)
         : Slot == 1 ? reinterpret_cast<void*>(&stub_add_ref)
         : Slot == 2 ? reinterpret_cast<void*>(&stub_release)
                     : reinterpret_cast<void*>(&stub_method<Slot>))...};
}

const auto kVtable = make_vtable(std::make_index_sequence<kSlots>{});
GameInputStub g_gameinput{kVtable.data()};

HRESULT __stdcall stub_qi(GameInputStub*, const IID& riid, void** out) {
    if (!out) return E_POINTER;
    // An interface pointer is handed back for any IID: the measured call site holds this object only to AddRef and
    // Release it, and the other v0 interfaces answer E_NOTIMPL on their slots rather than faulting on a null.
    (void)riid;
    *out = &g_gameinput;
    g_refs.fetch_add(1);
    return S_OK;
}

ULONG __stdcall stub_add_ref(GameInputStub*) { return g_refs.fetch_add(1) + 1; }

ULONG __stdcall stub_release(GameInputStub*) { return g_refs.fetch_sub(1) - 1; }

// ---- the device hub ---------------------------------------------------------------------------------------------

constexpr uint32_t kKinds =
    GameInputKindControllerAxis | GameInputKindControllerButton | GameInputKindGamepad | GameInputKindUiNavigation;
constexpr uint32_t kConnectedStatus = GameInputDeviceConnected | GameInputDeviceInputEnabled | GameInputDeviceOutputEnabled;
constexpr uint32_t kAxisCount = 6;     // LX, LY, RX, RY, LT, RT, as the SDL3 shim reports them
constexpr uint32_t kButtonCount = 15;  // SDL's standard gamepad buttons, numbered as the script's Button::sdl
constexpr size_t kHistoryDepth = 256;  // readings kept per device; a fork found 64 too small
constexpr size_t kMaxPendingReadingEvents = 1024;
constexpr char kScriptPadName[] = "Scripted pad (Leonida)";

uint64_t qpc_now_us() {
    static const LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart / freq.QuadPart * 1'000'000 + c.QuadPart % freq.QuadPart * 1'000'000 / freq.QuadPart);
}
std::atomic<mac::ClockFn> g_clock{&qpc_now_us};
uint64_t now_us() { return g_clock.load()(); }

bool same_guid(const GUID& a, const GUID& b) { return std::memcmp(&a, &b, sizeof(GUID)) == 0; }

ULONG release_floor(std::atomic<ULONG>& refs) {  // objects kept for the process: never freed
    ULONG r = refs.load();
    while (r > 1 && !refs.compare_exchange_weak(r, r - 1)) {}
    return r > 1 ? r - 1 : 1;
}

// Logs the first call of every served and unserved slot of IGameInput, IGameInputDevice, IGameInputReading and the
// dispatcher, once per call site: `first call of X` for a served slot, `first call of X: not served, returns R` for one
// that fails or answers with nothing.
void log_first_call(const char* name, const char* unserved_result) {
    if (unserved_result)
        std::fprintf(stderr, "[rt] gameinput: first call of %s: not served, returns %s (logged once)\n", name, unserved_result);
    else
        std::fprintf(stderr, "[rt] gameinput: first call of %s\n", name);
}

#define GI_FIRST(name, unserved_result)                                                                            \
    do {                                                                                                           \
        static std::atomic<bool> gi_seen_;                                                                         \
        if (!gi_seen_.load(std::memory_order_relaxed) && !gi_seen_.exchange(true)) log_first_call(name, unserved_result); \
    } while (0)
#define GI_CALLED(name) GI_FIRST(name, nullptr)
#define GI_UNSERVED(name, result) GI_FIRST(name, result)

template <size_t Slot>
void not_served(const char* what) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "[rt] gameinput: slot %zu (%s) is not served; E_NOTIMPL (logged once)\n", Slot, what);
}

GameInputGamepadState to_gi(const mac::PadState& s) {
    GameInputGamepadState g{};
    g.buttons = s.buttons;
    g.leftTrigger = s.lt;
    g.rightTrigger = s.rt;
    g.leftThumbstickX = s.lx;
    g.leftThumbstickY = s.ly;
    g.rightThumbstickX = s.rx;
    g.rightThumbstickY = s.ry;
    return g;
}

// SDL's standard gamepad button numbering, which GetControllerButtonState uses: the script's table plus the clicks.
void controller_buttons(const mac::PadState& s, bool guide, bool out[kButtonCount]) {
    for (uint32_t i = 0; i < kButtonCount; ++i) out[i] = false;
    for (const script::Button& b : script::kButtons) out[b.sdl] = (s.buttons & b.bit) != 0;
    out[5] = guide;
    out[7] = (s.buttons & GameInputGamepadLeftThumbstick) != 0;
    out[8] = (s.buttons & GameInputGamepadRightThumbstick) != 0;
}

enum class Source { Script, Controller, Keyboard, Aggregate };

struct Device;
class Reading;

class Reading final : public IGameInputReading {
public:
    Reading(Device* device, const mac::PadState& state, bool guide, uint64_t seq, uint64_t order, uint64_t ts)
        : device_(device), state_(state), guide_(guide), seq_(seq), order_(order), ts_(ts) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (same_guid(riid, __uuidof(IUnknown)) || same_guid(riid, IID_IGameInputReading)) {
            AddRef();
            *out = static_cast<IGameInputReading*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }

    uint32_t STDMETHODCALLTYPE GetInputKind() override {
        GI_CALLED("IGameInputReading::GetInputKind");
        return kKinds;
    }
    uint64_t STDMETHODCALLTYPE GetSequenceNumber(uint32_t) override {
        GI_CALLED("IGameInputReading::GetSequenceNumber");
        return seq_;
    }
    uint64_t STDMETHODCALLTYPE GetTimestamp() override {
        GI_CALLED("IGameInputReading::GetTimestamp");
        return ts_;
    }
    void STDMETHODCALLTYPE GetDevice(IGameInputDevice** device) override;
    bool STDMETHODCALLTYPE GetRawReport(void** report) override {
        GI_UNSERVED("IGameInputReading::GetRawReport", "false");
        if (report) *report = nullptr;
        return false;
    }
    uint32_t STDMETHODCALLTYPE GetControllerAxisCount() override {
        GI_CALLED("IGameInputReading::GetControllerAxisCount");
        return kAxisCount;
    }
    uint32_t STDMETHODCALLTYPE GetControllerAxisState(uint32_t count, float* out) override {
        GI_CALLED("IGameInputReading::GetControllerAxisState");
        if (!out) return 0;
        // 0..1 with 0.5 at rest for the sticks, as the SDL3 shim reports them.
        const float axes[kAxisCount] = {(state_.lx + 1) * 0.5f, (state_.ly + 1) * 0.5f, (state_.rx + 1) * 0.5f,
                                        (state_.ry + 1) * 0.5f, state_.lt,              state_.rt};
        const uint32_t n = std::min(count, kAxisCount);
        std::memcpy(out, axes, n * sizeof(float));
        return n;
    }
    uint32_t STDMETHODCALLTYPE GetControllerButtonCount() override {
        GI_CALLED("IGameInputReading::GetControllerButtonCount");
        return kButtonCount;
    }
    uint32_t STDMETHODCALLTYPE GetControllerButtonState(uint32_t count, bool* out) override {
        GI_CALLED("IGameInputReading::GetControllerButtonState");
        if (!out) return 0;
        bool buttons[kButtonCount];
        controller_buttons(state_, guide_, buttons);
        const uint32_t n = std::min(count, kButtonCount);
        std::memcpy(out, buttons, n * sizeof(bool));
        return n;
    }
    uint32_t STDMETHODCALLTYPE GetControllerSwitchCount() override {
        GI_UNSERVED("IGameInputReading::GetControllerSwitchCount", "0");
        return 0;
    }
    uint32_t STDMETHODCALLTYPE GetControllerSwitchState(uint32_t, GameInputSwitchPosition*) override {
        GI_UNSERVED("IGameInputReading::GetControllerSwitchState", "0");
        return 0;
    }
    uint32_t STDMETHODCALLTYPE GetKeyCount() override {
        GI_UNSERVED("IGameInputReading::GetKeyCount", "0");
        return 0;
    }
    uint32_t STDMETHODCALLTYPE GetKeyState(uint32_t, void*) override {
        GI_UNSERVED("IGameInputReading::GetKeyState", "0");
        return 0;
    }
    bool STDMETHODCALLTYPE GetMouseState(void*) override {
        GI_UNSERVED("IGameInputReading::GetMouseState", "false");
        return false;
    }
    uint32_t STDMETHODCALLTYPE GetTouchCount() override {
        GI_UNSERVED("IGameInputReading::GetTouchCount", "0");
        return 0;
    }
    uint32_t STDMETHODCALLTYPE GetTouchState(uint32_t, void*) override {
        GI_UNSERVED("IGameInputReading::GetTouchState", "0");
        return 0;
    }
    bool STDMETHODCALLTYPE GetMotionState(void*) override {
        GI_UNSERVED("IGameInputReading::GetMotionState", "false");
        return false;
    }
    bool STDMETHODCALLTYPE GetArcadeStickState(void*) override {
        GI_UNSERVED("IGameInputReading::GetArcadeStickState", "false");
        return false;
    }
    bool STDMETHODCALLTYPE GetFlightStickState(void*) override {
        GI_UNSERVED("IGameInputReading::GetFlightStickState", "false");
        return false;
    }
    bool STDMETHODCALLTYPE GetGamepadState(GameInputGamepadState* state) override {
        GI_CALLED("IGameInputReading::GetGamepadState");
        if (!state) return false;
        *state = to_gi(state_);
        return true;
    }
    bool STDMETHODCALLTYPE GetRacingWheelState(void*) override {
        GI_UNSERVED("IGameInputReading::GetRacingWheelState", "false");
        return false;
    }
    bool STDMETHODCALLTYPE GetUiNavigationState(GameInputUiNavigationState* state) override {
        GI_CALLED("IGameInputReading::GetUiNavigationState");
        if (!state) return false;
        state->buttons = script::ui_navigation(to_gi(state_));
        return true;
    }

    const mac::PadState& pad() const { return state_; }
    bool guide() const { return guide_; }
    uint64_t seq() const { return seq_; }
    uint64_t order() const { return order_; }
    uint64_t timestamp() const { return ts_; }
    Device* device() const { return device_; }

private:
    std::atomic<ULONG> refs_{1};
    Device* device_;
    mac::PadState state_;
    bool guide_;
    uint64_t seq_, order_, ts_;
};

void fnv_fill(const std::string& key, uint8_t* out32) {
    for (int lane = 0; lane < 4; ++lane) {
        uint64_t h = 0xcbf29ce484222325ull ^ (0x9e3779b97f4a7c15ull * static_cast<uint64_t>(lane + 1));
        for (unsigned char c : key) h = (h ^ c) * 0x100000001b3ull;
        std::memcpy(out32 + lane * 8, &h, 8);
    }
}

// A device lives for the process. The hub's lock guards every member below `refs_`.
struct Device final : IGameInputDevice {
    Device(Source source, std::string key, const mac::PadDesc& desc) : source(source), key(std::move(key)), name(desc.name) {
        wireless = desc.wireless;
        rumble_motors = desc.rumble_motors;
        GameInputDeviceInfo& i = info_;
        i.infoSize = sizeof(GameInputDeviceInfo);
        i.vendorId = desc.vendor;
        i.productId = desc.product;
        i.usage = {0x01, 0x05};  // HID generic desktop / game pad
        fnv_fill(this->key, i.deviceId.value);
        i.deviceRootId = i.deviceId;
        i.deviceFamily = source == Source::Aggregate ? GameInputFamilyAggregate : GameInputFamilyXboxOne;
        i.capabilities = wireless ? GameInputDeviceCapabilityWireless : GameInputDeviceCapabilityNone;
        i.supportedInput = source == Source::Aggregate ? GameInputKindGamepad : kKinds;
        i.supportedRumbleMotors = rumble_motors;
        i.controllerAxisCount = kAxisCount;
        i.controllerButtonCount = kButtonCount;
        for (uint32_t a = 0; a < kAxisCount; ++a) {
            axis_info_[a].mappedInputKinds = GameInputKindGamepad;
            axis_info_[a].isContinuous = true;
            axis_info_[a].hasRestValue = true;
            axis_info_[a].restValue = a < 4 ? 0.5f : 0.0f;
            axis_info_[a].resolution = 65536;
            axis_info_[a].legacyHidIndex = axis_info_[a].legacyDInputIndex = static_cast<uint16_t>(a);
        }
        axis_info_[4].label = GameInputLabelXboxLeftTrigger;
        axis_info_[5].label = GameInputLabelXboxRightTrigger;
        const GameInputLabel labels[kButtonCount] = {
            GameInputLabelXboxA,            GameInputLabelXboxB,          GameInputLabelXboxX,
            GameInputLabelXboxY,            GameInputLabelXboxView,       GameInputLabelXboxGuide,
            GameInputLabelXboxMenu,         GameInputLabelXboxLeftStickButton, GameInputLabelXboxRightStickButton,
            GameInputLabelXboxLeftShoulder, GameInputLabelXboxRightShoulder, GameInputLabelXboxDPadUp,
            GameInputLabelXboxDPadDown,     GameInputLabelXboxDPadLeft,   GameInputLabelXboxDPadRight};
        for (uint32_t b = 0; b < kButtonCount; ++b) {
            button_info_[b].mappedInputKinds = GameInputKindGamepad;
            button_info_[b].label = labels[b];
            button_info_[b].legacyHidIndex = button_info_[b].legacyDInputIndex = static_cast<uint16_t>(b);
        }
        gamepad_info_ = {GameInputLabelXboxMenu,        GameInputLabelXboxView,           GameInputLabelXboxA,
                         GameInputLabelXboxB,           GameInputLabelXboxX,              GameInputLabelXboxY,
                         GameInputLabelXboxDPadUp,      GameInputLabelXboxDPadDown,       GameInputLabelXboxDPadLeft,
                         GameInputLabelXboxDPadRight,   GameInputLabelXboxLeftShoulder,   GameInputLabelXboxRightShoulder,
                         GameInputLabelXboxLeftStickButton, GameInputLabelXboxRightStickButton};
        const GameInputLabel none = GameInputLabelNone;
        ui_info_ = {GameInputLabelXboxMenu,       GameInputLabelXboxView,          GameInputLabelXboxA,
                    GameInputLabelXboxB,          GameInputLabelXboxDPadUp,        GameInputLabelXboxDPadDown,
                    GameInputLabelXboxDPadLeft,   GameInputLabelXboxDPadRight,     GameInputLabelXboxX,
                    GameInputLabelXboxY,          GameInputLabelXboxLeftStickButton, GameInputLabelXboxRightStickButton,
                    GameInputLabelXboxLeftTrigger, GameInputLabelXboxRightTrigger, GameInputLabelXboxLeftShoulder,
                    GameInputLabelXboxRightShoulder, none, none, none, none, GameInputLabelXboxGuide};
        i.controllerAxisInfo = axis_info_;
        i.controllerButtonInfo = button_info_;
        i.gamepadInfo = &gamepad_info_;
        i.uiNavigationInfo = &ui_info_;
        const uint32_t name_size = static_cast<uint32_t>(name.size());
        display_name_ = {name_size, name_size, name.c_str()};
        i.displayName = &display_name_;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (same_guid(riid, __uuidof(IUnknown)) || same_guid(riid, IID_IGameInputDevice)) {
            AddRef();
            *out = static_cast<IGameInputDevice*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return release_floor(refs_); }

    const GameInputDeviceInfo* STDMETHODCALLTYPE GetDeviceInfo() override {
        GI_CALLED("IGameInputDevice::GetDeviceInfo");
        return &info_;
    }
    uint32_t STDMETHODCALLTYPE GetDeviceStatus() override {
        GI_CALLED("IGameInputDevice::GetDeviceStatus");
        return status.load();
    }
    void STDMETHODCALLTYPE GetBatteryState(GameInputBatteryState* state) override;
    // No force-feedback motors: every motor index is out of range.
    HRESULT STDMETHODCALLTYPE CreateForceFeedbackEffect(uint32_t, const void*, void** effect) override {
        GI_UNSERVED("IGameInputDevice::CreateForceFeedbackEffect", "E_INVALIDARG (no force-feedback motors)");
        if (effect) *effect = nullptr;
        return E_INVALIDARG;
    }
    bool STDMETHODCALLTYPE IsForceFeedbackMotorPoweredOn(uint32_t) override {
        GI_UNSERVED("IGameInputDevice::IsForceFeedbackMotorPoweredOn", "false");
        return false;
    }
    void STDMETHODCALLTYPE SetForceFeedbackMotorGain(uint32_t, float) override {
        GI_UNSERVED("IGameInputDevice::SetForceFeedbackMotorGain", "nothing (void, ignored)");
    }
    HRESULT STDMETHODCALLTYPE SetHapticMotorState(uint32_t, const void*) override {
        GI_UNSERVED("IGameInputDevice::SetHapticMotorState", "E_INVALIDARG (no haptic motors)");
        return E_INVALIDARG;
    }
    void STDMETHODCALLTYPE SetRumbleState(const GameInputRumbleParams* params) override;
    void STDMETHODCALLTYPE SetInputSynchronizationState(bool) override {
        GI_UNSERVED("IGameInputDevice::SetInputSynchronizationState", "nothing (void, ignored)");
    }
    void STDMETHODCALLTYPE SendInputSynchronizationHint() override {
        GI_UNSERVED("IGameInputDevice::SendInputSynchronizationHint", "nothing (void, ignored)");
    }
    void STDMETHODCALLTYPE PowerOff() override {
        GI_UNSERVED("IGameInputDevice::PowerOff", "nothing (void, ignored; the pad stays on)");
    }
    HRESULT STDMETHODCALLTYPE CreateRawDeviceReport(uint32_t, int32_t, void** report) override {
        GI_UNSERVED("IGameInputDevice::CreateRawDeviceReport", "E_NOTIMPL (raw device access)");
        return no_raw(report);
    }
    HRESULT STDMETHODCALLTYPE GetRawDeviceFeature(uint32_t, void** report) override {
        GI_UNSERVED("IGameInputDevice::GetRawDeviceFeature", "E_NOTIMPL (raw device access)");
        return no_raw(report);
    }
    HRESULT STDMETHODCALLTYPE SetRawDeviceFeature(void*) override {
        GI_UNSERVED("IGameInputDevice::SetRawDeviceFeature", "E_NOTIMPL (raw device access)");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SendRawDeviceOutput(void*) override {
        GI_UNSERVED("IGameInputDevice::SendRawDeviceOutput", "E_NOTIMPL (raw device access)");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SendRawDeviceOutputWithResponse(void*, void** response) override {
        GI_UNSERVED("IGameInputDevice::SendRawDeviceOutputWithResponse", "E_NOTIMPL (raw device access)");
        return no_raw(response);
    }
    HRESULT STDMETHODCALLTYPE ExecuteRawDeviceIoControl(uint32_t, size_t, const void*, size_t, void*, size_t* size) override {
        GI_UNSERVED("IGameInputDevice::ExecuteRawDeviceIoControl", "E_NOTIMPL (raw device access)");
        if (size) *size = 0;
        return E_NOTIMPL;
    }
    bool STDMETHODCALLTYPE AcquireExclusiveRawDeviceAccess(uint64_t) override {
        GI_UNSERVED("IGameInputDevice::AcquireExclusiveRawDeviceAccess", "false");
        return false;
    }
    void STDMETHODCALLTYPE ReleaseExclusiveRawDeviceAccess() override {
        GI_UNSERVED("IGameInputDevice::ReleaseExclusiveRawDeviceAccess", "nothing (void, ignored)");
    }

    Source source;
    std::string key;
    std::string name;
    uint64_t handle = 0;
    bool hw_present = false;
    bool wireless = false;
    uint32_t rumble_motors = 0;
    std::atomic<uint32_t> status{GameInputDeviceNoStatus};
    std::deque<Reading*> history;  // one reference each, oldest first; the newest is the current reading
    uint64_t seq = 0;
    uint64_t evicted_order = 0;  // order of the newest reading dropped from history
    mac::PadState last;
    bool last_guide = false;
    GameInputRumbleParams rumble{};

private:
    static HRESULT no_raw(void** out) {
        if (out) *out = nullptr;
        return E_NOTIMPL;
    }
    std::atomic<ULONG> refs_{1};
    GameInputDeviceInfo info_{};
    GameInputControllerAxisInfo axis_info_[kAxisCount]{};
    GameInputControllerButtonInfo button_info_[kButtonCount]{};
    GameInputGamepadInfo gamepad_info_{};
    GameInputUiNavigationInfo ui_info_{};
    GameInputString display_name_{};
};

void Reading::GetDevice(IGameInputDevice** device) {
    GI_CALLED("IGameInputReading::GetDevice");
    if (!device) return;
    device_->AddRef();
    *device = device_;
}

enum class CbType { Device, Reading, Guide, KeyboardLayout };

struct Callback {
    GameInputCallbackToken token;
    CbType type;
    void* context;
    void* fn;
    bool active;
    Device* device;  // null or an aggregate: any device
    uint32_t kind;
    uint32_t status_filter;
    float threshold;
    bool overrun;  // reading events were dropped since the last delivery
};

struct Event {
    GameInputCallbackToken token;
    CbType type;
    Device* device;
    uint64_t timestamp;
    uint32_t current, previous;
    Reading* reading;  // one reference, reading events
    bool pressed;
};

struct Hub final : mac::Sink {
    enum class KeyboardMode { Auto, On, Off };

    std::recursive_mutex mu;          // everything below
    std::recursive_mutex deliver_mu;  // held while callbacks run; UnregisterCallback waits on it
    std::atomic<DWORD> delivering_thread{0};
    std::vector<Device*> devices;  // never shrinks; handle = index + 1
    Device* aggregate = nullptr;
    Device* script_pad = nullptr;
    script::Script script;
    uint64_t origin_us = 0;
    bool log_presses = true;
    uint32_t logged_held = 0;
    uint64_t order = 0;  // across devices, one per reading
    std::vector<Callback> callbacks;
    std::deque<Event> events;
    GameInputCallbackToken next_token = 1;
    int dispatchers = 0;
    HANDLE wait_event = nullptr;    // auto-reset, signalled when a dispatcher has work
    HANDLE worker_event = nullptr;  // auto-reset, wakes the delivery thread
    bool worker_started = false;
    KeyboardMode keyboard_mode = KeyboardMode::Auto;
    bool rumble_enabled = true;
    std::unique_ptr<mac::Backend> backend;
    mac::RumbleObserver rumble_observer = nullptr;

    std::vector<const void*> logged_polls;
    void log_poll_form(IGameInputDevice* asked, Device* dev) {
        const void* key = asked;
        for (const void* k : logged_polls)
            if (k == key) return;
        logged_polls.push_back(key);
        std::fprintf(stderr, "[rt] gameinput: GetCurrentReading asked for %s\n",
                     !asked ? "no device (the newest non-idle reading of any connected device)"
                            : dev ? dev->name.c_str() : "an unknown device");
    }

    Device* find_handle(uint64_t handle) {
        return handle && handle <= devices.size() ? devices[handle - 1] : nullptr;
    }

    bool kind_matches(uint32_t requested) const { return requested == GameInputKindUnknown || (requested & kKinds); }

    std::vector<Device*> candidates(Device* filter) {
        std::vector<Device*> out;
        if (filter && filter->source != Source::Aggregate) {
            out.push_back(filter);
            return out;
        }
        for (Device* d : devices)
            if (d->source != Source::Aggregate && d->status.load() != GameInputDeviceNoStatus) out.push_back(d);
        return out;
    }

    // Under mu.
    void post(const Event& e) {
        events.push_back(e);
        if (dispatchers > 0) SetEvent(wait_event);
        else wake_worker();
    }

    void wake_worker();
    void deliver();

    bool callback_wants(const Callback& c, CbType type, Device* d) const {
        return c.active && c.type == type && (!c.device || c.device->source == Source::Aggregate || c.device == d);
    }

    // Under mu. The reading callbacks hear every new reading of a matching device (past the threshold, if any).
    void announce_reading(Device& d, Reading* r, const mac::PadState& previous) {
        for (Callback& c : callbacks) {
            if (!callback_wants(c, CbType::Reading, &d) || !(c.kind == GameInputKindUnknown || (c.kind & kKinds))) continue;
            if (c.threshold > 0) {
                const mac::PadState& s = r->pad();
                const float delta = std::max({std::fabs(s.lt - previous.lt), std::fabs(s.rt - previous.rt),
                                              std::fabs(s.lx - previous.lx), std::fabs(s.ly - previous.ly),
                                              std::fabs(s.rx - previous.rx), std::fabs(s.ry - previous.ry)});
                if (s.buttons == previous.buttons && delta < c.threshold) continue;
            }
            size_t pending = 0;
            for (const Event& e : events)
                if (e.type == CbType::Reading) ++pending;
            if (pending >= kMaxPendingReadingEvents) {
                c.overrun = true;
                continue;
            }
            r->AddRef();
            post({c.token, CbType::Reading, &d, r->timestamp(), 0, 0, r, c.overrun});
            c.overrun = false;
        }
    }

    // Under mu. Takes a reading when the state differs from the last.
    void push_reading(Device& d, const mac::PadState& state, bool guide, uint64_t now) {
        const bool first = d.history.empty();
        if (!first && state == d.last && guide == d.last_guide) return;
        const mac::PadState previous = d.last;
        const bool guide_was = d.last_guide;
        d.last = state;
        d.last_guide = guide;
        auto* r = new Reading(&d, state, guide, ++d.seq, ++order, now);
        d.history.push_back(r);
        while (d.history.size() > kHistoryDepth) {
            d.evicted_order = d.history.front()->order();
            d.history.front()->Release();
            d.history.pop_front();
        }
        announce_reading(d, r, previous);
        if (guide != guide_was)
            for (Callback& c : callbacks)
                if (callback_wants(c, CbType::Guide, &d)) post({c.token, CbType::Guide, &d, now, 0, 0, nullptr, guide});
    }

    // Under mu. Changes a device's status, reports it, and keeps the history in step.
    void set_status(Device& d, uint32_t status, uint64_t now) {
        const uint32_t previous = d.status.exchange(status);
        if (previous == status) return;
        if (status != GameInputDeviceNoStatus) {
            if (d.history.empty()) push_reading(d, d.last, d.last_guide, now);
        } else {
            for (Reading* r : d.history) r->Release();
            d.history.clear();
            d.evicted_order = order;
            d.last = {};
            d.last_guide = false;
        }
        for (const Callback& c : callbacks)
            if (callback_wants(c, CbType::Device, &d) && (c.kind == GameInputKindUnknown || (c.kind & kKinds)) &&
                ((status ^ previous) & c.status_filter))
                post({c.token, CbType::Device, &d, now, status, previous, nullptr, false});
        std::fprintf(stderr, "[rt] gameinput: %s \"%s\" (%s)\n",
                     status != GameInputDeviceNoStatus ? "connected" : "disconnected", d.name.c_str(),
                     d.source == Source::Keyboard ? "keyboard pad" : d.source == Source::Script ? "script" : "controller");
    }

    // Under mu. The keyboard pad stands in while no controller is attached (mode Auto).
    void refresh_keyboard(uint64_t now) {
        bool controller = false;
        for (Device* d : devices)
            if (d->source == Source::Controller && d->hw_present) controller = true;
        for (Device* d : devices) {
            if (d->source != Source::Keyboard) continue;
            const bool want = d->hw_present && (keyboard_mode == KeyboardMode::On ||
                                                (keyboard_mode == KeyboardMode::Auto && !controller));
            set_status(*d, want ? kConnectedStatus : GameInputDeviceNoStatus, now);
        }
    }

    // mac::Sink
    uint64_t connect(const mac::PadDesc& desc) override {
        std::lock_guard lock(mu);
        const uint64_t now = now_us();
        Device* d = nullptr;
        for (Device* c : devices)
            if (c->key == desc.key && !c->hw_present && c->source != Source::Script && c->source != Source::Aggregate) d = c;
        if (!d) {
            std::string key = desc.key;
            for (int n = 1;; ++n) {
                bool taken = false;
                for (Device* c : devices) taken |= c->key == key;
                if (!taken) break;
                key = desc.key + "+" + std::to_string(n);
            }
            d = new Device(desc.kind == mac::PadKind::Keyboard ? Source::Keyboard : Source::Controller, key, desc);
            devices.push_back(d);
            d->handle = devices.size();
        }
        d->hw_present = true;
        if (d->source == Source::Keyboard) refresh_keyboard(now);
        else {
            set_status(*d, kConnectedStatus | (d->wireless ? GameInputDeviceWireless : 0), now);
            refresh_keyboard(now);
        }
        return d->handle;
    }

    void disconnect(uint64_t handle) override {
        std::lock_guard lock(mu);
        Device* d = find_handle(handle);
        if (!d || d->source == Source::Script || !d->hw_present) return;
        d->hw_present = false;
        const uint64_t now = now_us();
        set_status(*d, GameInputDeviceNoStatus, now);
        refresh_keyboard(now);
    }

    void state(uint64_t handle, const mac::PadState& s, bool guide) override {
        std::lock_guard lock(mu);
        Device* d = find_handle(handle);
        if (!d || d->source == Source::Script || d->status.load() == GameInputDeviceNoStatus) return;
        push_reading(*d, s, guide, now_us());
    }

    // Under mu. The scripted pad reads its timeline when the title asks. A press is held for the script's time and
    // then until the title has asked for it on min_polls separate frames (calls at least kPollGapUs apart), at most
    // kMaxHoldSeconds: the title polls once per frame and frames stall, so a fixed 300 ms can be missed or seen once.
    uint32_t min_polls = 4;  // GTA6RT_SCRIPT_MIN_POLLS; 0 is the exact timeline
    static constexpr uint64_t kPollGapUs = 8000;
    static constexpr double kMaxHoldSeconds = 1.5;
    static constexpr double kLateSeconds = 10.0;  // a press the title did not ask for in its hold is still delivered
    std::vector<bool> served;
    std::vector<bool> announced;
    script::Held sticky;
    double press_start_s = 0;
    uint32_t press_polls = 0;
    uint64_t last_poll_us = 0;
    bool press_active = false;
    uint64_t press_gets = 0;

    void refresh_script(uint64_t now) {
        if (!script_pad) return;
        Device& d = *script_pad;
        const double seconds = now > origin_us ? (now - origin_us) / 1e6 : 0.0;
        script::Held held = script::held_at(script, seconds);
        if (log_presses) {
            announced.resize(script.presses.size(), false);
            for (size_t i = 0; i < script.presses.size(); ++i) {
                const script::Press& p = script.presses[i];
                if (announced[i] || p.button || !script::holds_at(p, seconds)) continue;
                announced[i] = true;
                std::fprintf(stderr, "[rt] gameinput: scripted %s@%.1f held from %.1f s for %.1f s\n", p.name.c_str(), p.at,
                             seconds, p.hold);
            }
        }
        if (min_polls > 0) {
            served.resize(script.presses.size(), false);
            for (size_t i = 0; i < script.presses.size(); ++i) {
                const script::Press& p = script.presses[i];
                if (script::holds_at(p, seconds)) served[i] = true;
                else if (!served[i] && seconds >= p.at + p.hold && seconds < p.at + kLateSeconds) {
                    served[i] = true;
                    script::hold(held, p);
                    if (log_presses)
                        std::fprintf(stderr,
                                     "[rt] gameinput: scripted press at %.1f s delivered late, at %.1f s: the title did not ask during its %.1f s hold\n",
                                     p.at, seconds, p.hold);
                }
            }
        }
        if (held.any()) {
            sticky = held;
        } else if (press_active && press_polls < min_polls && seconds - press_start_s < kMaxHoldSeconds) {
            held = sticky;
        } else {
            sticky = {};
        }
        const bool pressed = held.any();
        if (pressed) {
            if (!press_active) {
                press_active = true;
                press_start_s = seconds;
                press_polls = 0;
                press_gets = 0;
                last_poll_us = 0;
            }
            ++press_gets;
            if (!last_poll_us || now - last_poll_us >= kPollGapUs) {
                ++press_polls;
                last_poll_us = now;
            }
        } else if (press_active) {
            press_active = false;
            if (log_presses)
                std::fprintf(stderr, "[rt] gameinput: scripted release at %.1f s: held %.0f ms, asked on %u frame(s) (%llu call(s))\n",
                             seconds, (seconds - press_start_s) * 1000.0, press_polls,
                             static_cast<unsigned long long>(press_gets));
        }
        const mac::PadState state = script::pad_of(held);
        const bool first = d.history.empty();
        if (!first && state == d.last) return;
        script::log_scripted(state.buttons, &logged_held, seconds);
        if (log_presses && !first)
            script::log_presses(state.buttons & ~d.last.buttons, d.last.ry, state.ry, seconds, kScriptPadName);
        push_reading(d, state, false, now);
    }

    void refresh(uint64_t now) { refresh_script(now); }
};

Hub& hub() {
    static Hub* h = new Hub;  // never destroyed: a title thread may still read during exit
    return *h;
}

void Hub::wake_worker() {
    if (!worker_started) {
        worker_started = true;
        rt::gdk::spawn_detached(
            [] {
                Hub& h = hub();
                for (;;) {
                    WaitForSingleObject(h.worker_event, INFINITE);
                    bool owned;
                    {
                        std::lock_guard lock(h.mu);
                        owned = h.dispatchers == 0;
                    }
                    if (owned) h.deliver();
                }
            },
            "gameinput");
    }
    SetEvent(worker_event);
}


void Hub::deliver() {
    std::lock_guard dl(deliver_mu);
    std::deque<Event> batch;
    {
        std::lock_guard lock(mu);
        batch.swap(events);
    }
    delivering_thread = GetCurrentThreadId();
    for (Event& e : batch) {
        Callback c{};
        bool found = false;
        {
            std::lock_guard lock(mu);
            for (const Callback& cb : callbacks)
                if (cb.token == e.token && cb.active) {
                    c = cb;
                    found = true;
                }
        }
        if (found && c.fn) {
            switch (e.type) {
                case CbType::Device:
                    reinterpret_cast<GameInputDeviceCallback>(c.fn)(c.token, c.context, e.device, e.timestamp, e.current,
                                                                    e.previous);
                    break;
                case CbType::Reading:
                    reinterpret_cast<GameInputReadingCallback>(c.fn)(c.token, c.context, e.reading, e.pressed);
                    break;
                case CbType::Guide:
                    reinterpret_cast<GameInputGuideButtonCallback>(c.fn)(c.token, c.context, e.device, e.timestamp,
                                                                         e.pressed);
                    break;
                case CbType::KeyboardLayout:
                    break;
            }
        }
        if (e.reading) e.reading->Release();
    }
    delivering_thread = 0;
}

void Device::GetBatteryState(GameInputBatteryState* state) {
    GI_CALLED("IGameInputDevice::GetBatteryState");
    if (!state) return;
    *state = {0, 0, 0, 0, GameInputBatteryUnknown};
    Hub& h = hub();
    mac::Backend* backend;
    uint64_t handle;
    {
        std::lock_guard lock(h.mu);
        backend = h.backend.get();
        handle = this->handle;
        if (!hw_present) return;
    }
    mac::BatteryInfo b;
    if (backend && source == Source::Controller && backend->battery(handle, &b))
        *state = {0, 0, b.level, 1.0f, static_cast<GameInputBatteryStatus>(b.state)};
}

void Device::SetRumbleState(const GameInputRumbleParams* params) {
    GI_CALLED("IGameInputDevice::SetRumbleState");
    if (!params || source == Source::Aggregate) return;
    Hub& h = hub();
    const auto unit = [](float v) { return v != v ? 0.0f : v < 0 ? 0.0f : v > 1 ? 1.0f : v; };
    const mac::RumbleLevels levels{unit(params->lowFrequency), unit(params->highFrequency), unit(params->leftTrigger),
                                   unit(params->rightTrigger)};
    mac::Backend* backend;
    mac::RumbleObserver observer;
    {
        std::lock_guard lock(h.mu);
        if (!h.rumble_enabled) return;
        if (levels.low == rumble.lowFrequency && levels.high == rumble.highFrequency &&
            levels.left_trigger == rumble.leftTrigger && levels.right_trigger == rumble.rightTrigger)
            return;
        rumble = {levels.low, levels.high, levels.left_trigger, levels.right_trigger};
        backend = hw_present && rumble_motors ? h.backend.get() : nullptr;
        observer = h.rumble_observer;
    }
    if (observer) observer(handle, levels);
    if (backend) backend->set_rumble(handle, levels);
}

class Dispatcher final : public IGameInputDispatcher {
public:
    Dispatcher() {
        std::lock_guard lock(hub().mu);
        ++hub().dispatchers;
        if (!hub().events.empty()) SetEvent(hub().wait_event);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (same_guid(riid, __uuidof(IUnknown)) || same_guid(riid, IID_IGameInputDispatcher)) {
            AddRef();
            *out = static_cast<IGameInputDispatcher*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --refs_;
        if (r == 0) {
            {
                std::lock_guard lock(hub().mu);
                if (--hub().dispatchers == 0 && !hub().events.empty()) hub().wake_worker();
            }
            delete this;
        }
        return r;
    }
    // The pending events are few, so all of them run whatever the quota.
    bool STDMETHODCALLTYPE Dispatch(uint64_t) override {
        GI_CALLED("IGameInputDispatcher::Dispatch");
        hub().deliver();
        std::lock_guard lock(hub().mu);
        return !hub().events.empty();
    }
    HRESULT STDMETHODCALLTYPE OpenWaitHandle(HANDLE* handle) override {
        GI_CALLED("IGameInputDispatcher::OpenWaitHandle");
        if (!handle) return E_POINTER;
        *handle = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), hub().wait_event, GetCurrentProcess(), handle, 0, FALSE,
                             DUPLICATE_SAME_ACCESS))
            return HRESULT_FROM_WIN32(GetLastError());
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
};

// Under hub().mu: the device object `p` names, or null when it is none of ours.
Device* resolve(IGameInputDevice* p) {
    Hub& h = hub();
    if (!p) return nullptr;
    if (p == static_cast<IGameInputDevice*>(h.aggregate)) return h.aggregate;
    for (Device* d : h.devices)
        if (static_cast<IGameInputDevice*>(d) == p) return d;
    return nullptr;
}

// GetNextReading (forward) and GetPreviousReading: the reading right after or right before the reference, across the
// devices searched (one device, or every connected one). A step across a reading dropped from a history searched is
// REFERENCE_READING_TOO_OLD, as in the SDL3 shim.
HRESULT find_reading(IGameInputReading* ref, uint32_t kind, IGameInputDevice* device, IGameInputReading** out,
                     bool forward) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!ref) return E_INVALIDARG;
    Hub& h = hub();
    if (!h.kind_matches(kind)) return GAMEINPUT_E_READING_NOT_FOUND;
    std::lock_guard lock(h.mu);
    Device* dev = resolve(device);
    if (device && !dev) return GAMEINPUT_E_DEVICE_NOT_FOUND;
    h.refresh(now_us());
    if (dev && dev->source != Source::Aggregate && dev->status.load() == GameInputDeviceNoStatus)
        return GAMEINPUT_E_DEVICE_DISCONNECTED;
    const auto* reference = static_cast<Reading*>(ref);
    const uint64_t r = reference->order();
    std::vector<Device*> searched = h.candidates(dev);
    uint64_t horizon = 0;
    for (Device* d : searched) horizon = std::max(horizon, d->evicted_order);
    if (!dev || dev->source == Source::Aggregate) horizon = std::max(horizon, reference->device()->evicted_order);
    if (forward ? horizon > r : horizon >= r) return GAMEINPUT_E_REFERENCE_READING_TOO_OLD;
    Reading* best = nullptr;
    for (Device* d : searched)
        for (Reading* c : d->history) {
            if (forward && c->order() > r) {
                if (!best || c->order() < best->order()) best = c;
                break;
            }
            if (!forward && c->order() < r && (!best || c->order() > best->order())) best = c;
        }
    if (!best) return GAMEINPUT_E_READING_NOT_FOUND;
    best->AddRef();
    *out = best;
    return S_OK;
}

// With no device named, a device that holds something wins over idle ones whatever their order, so a held key or an
// out-of-date scripted release does not hide the other.
bool is_idle(const Reading* r) { return r->pad() == mac::PadState{}; }

class MacInput final : public IGameInput {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (same_guid(riid, __uuidof(IUnknown)) || same_guid(riid, IID_IGameInput)) {
            AddRef();
            *out = static_cast<IGameInput*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return release_floor(refs_); }  // a process-wide singleton

    uint64_t STDMETHODCALLTYPE GetCurrentTimestamp() override {
        GI_CALLED("GetCurrentTimestamp");
        return now_us();
    }

    HRESULT STDMETHODCALLTYPE GetCurrentReading(uint32_t kind, IGameInputDevice* device, IGameInputReading** out) override {
        GI_CALLED("GetCurrentReading");
        if (!out) return E_POINTER;
        *out = nullptr;
        Hub& h = hub();
        if (!h.kind_matches(kind)) return GAMEINPUT_E_READING_NOT_FOUND;
        std::lock_guard lock(h.mu);
        Device* dev = resolve(device);
        if (device && !dev) return GAMEINPUT_E_DEVICE_NOT_FOUND;
        h.refresh(now_us());
        if (dev && dev->source != Source::Aggregate && dev->status.load() == GameInputDeviceNoStatus)
            return GAMEINPUT_E_DEVICE_DISCONNECTED;
        Reading* best = nullptr;
        for (Device* d : h.candidates(dev)) {
            if (d->history.empty()) continue;
            Reading* r = d->history.back();
            if (!best || (!is_idle(r) && is_idle(best)) || (is_idle(r) == is_idle(best) && r->order() > best->order()))
                best = r;
        }
        if (!best) return GAMEINPUT_E_READING_NOT_FOUND;
        h.log_poll_form(device, dev);
        best->AddRef();
        *out = best;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNextReading(IGameInputReading* ref, uint32_t kind, IGameInputDevice* device,
                                             IGameInputReading** out) override {
        GI_CALLED("GetNextReading");
        return find_reading(ref, kind, device, out, true);
    }
    HRESULT STDMETHODCALLTYPE GetPreviousReading(IGameInputReading* ref, uint32_t kind, IGameInputDevice* device,
                                                 IGameInputReading** out) override {
        GI_CALLED("GetPreviousReading");
        return find_reading(ref, kind, device, out, false);
    }
    // GameInput v0's own answer (see the SDL3 shim).
    HRESULT STDMETHODCALLTYPE GetTemporalReading(uint64_t, IGameInputDevice*, IGameInputReading** out) override {
        GI_UNSERVED("GetTemporalReading", "E_NOTIMPL (as GameInput v0 itself)");
        if (!out) return E_POINTER;
        *out = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE RegisterReadingCallback(IGameInputDevice* device, uint32_t kind, float threshold, void* context,
                                                      GameInputReadingCallback fn, GameInputCallbackToken* token) override {
        GI_CALLED("RegisterReadingCallback");
        if (token) *token = kInvalidCallbackToken;
        if (!fn) return E_INVALIDARG;
        Hub& h = hub();
        std::lock_guard lock(h.mu);
        Device* dev = resolve(device);
        if (device && !dev) return GAMEINPUT_E_DEVICE_NOT_FOUND;
        const GameInputCallbackToken t = h.next_token++;
        h.callbacks.push_back({t, CbType::Reading, context, reinterpret_cast<void*>(fn), true, dev, kind, 0,
                               threshold > 0 ? threshold : 0.0f, false});
        if (token) *token = t;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE RegisterDeviceCallback(IGameInputDevice* device, uint32_t kind, uint32_t status_filter,
                                                     GameInputEnumerationKind enumeration, void* context,
                                                     GameInputDeviceCallback fn, GameInputCallbackToken* token) override {
        GI_CALLED("RegisterDeviceCallback");
        if (token) *token = kInvalidCallbackToken;
        if (!fn) return E_INVALIDARG;
        Hub& h = hub();
        Callback c{};
        std::vector<std::pair<Device*, uint32_t>> report;
        {
            std::lock_guard lock(h.mu);
            Device* dev = resolve(device);
            if (device && !dev) return GAMEINPUT_E_DEVICE_NOT_FOUND;
            c = {h.next_token++, CbType::Device, context, reinterpret_cast<void*>(fn), true, dev, kind, status_filter, 0, false};
            h.callbacks.push_back(c);
            if (enumeration != GameInputNoEnumeration && h.kind_matches(kind)) {
                for (Device* d : h.devices) {
                    const uint32_t status = d->status.load();
                    if ((!dev || dev->source == Source::Aggregate || dev == d) && (status & status_filter))
                        report.push_back({d, status});
                }
                if (enumeration != GameInputBlockingEnumeration) {
                    for (const auto& [d, status] : report)
                        h.post({c.token, CbType::Device, d, now_us(), status, GameInputDeviceNoStatus, nullptr, false});
                }
            }
        }
        if (token) *token = c.token;
        // Blocking enumeration: the devices are reported before this returns, on the caller's thread.
        if (enumeration == GameInputBlockingEnumeration)
            for (const auto& [d, status] : report)
                reinterpret_cast<GameInputDeviceCallback>(fn)(c.token, context, d, now_us(), status, GameInputDeviceNoStatus);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RegisterGuideButtonCallback(IGameInputDevice* device, void* context,
                                                          GameInputGuideButtonCallback fn,
                                                          GameInputCallbackToken* token) override {
        GI_CALLED("RegisterGuideButtonCallback");
        return add(CbType::Guide, device, context, reinterpret_cast<void*>(fn), token);
    }
    // The pad has no keyboard layout: accepted, and it never runs.
    HRESULT STDMETHODCALLTYPE RegisterKeyboardLayoutCallback(IGameInputDevice* device, void* context,
                                                             GameInputKeyboardLayoutCallback fn,
                                                             GameInputCallbackToken* token) override {
        GI_UNSERVED("RegisterKeyboardLayoutCallback", "S_OK, but the callback never fires");
        return add(CbType::KeyboardLayout, device, context, reinterpret_cast<void*>(fn), token);
    }
    void STDMETHODCALLTYPE StopCallback(GameInputCallbackToken token) override {
        GI_CALLED("StopCallback");
        std::lock_guard lock(hub().mu);
        for (Callback& c : hub().callbacks)
            if (c.token == token) c.active = false;
    }
    // Removes the callback and, unless called from inside a callback, waits for one that is running to return.
    bool STDMETHODCALLTYPE UnregisterCallback(GameInputCallbackToken token, uint64_t) override {
        GI_CALLED("UnregisterCallback");
        Hub& s = hub();
        bool found = false;
        {
            std::lock_guard lock(s.mu);
            auto it = std::find_if(s.callbacks.begin(), s.callbacks.end(),
                                   [token](const Callback& c) { return c.token == token; });
            if (it != s.callbacks.end()) {
                s.callbacks.erase(it);
                found = true;
            }
        }
        if (found && s.delivering_thread != GetCurrentThreadId()) std::lock_guard wait(s.deliver_mu);
        return found;
    }
    HRESULT STDMETHODCALLTYPE CreateDispatcher(IGameInputDispatcher** out) override {
        GI_CALLED("CreateDispatcher");
        if (!out) return E_POINTER;
        *out = new Dispatcher;
        return S_OK;
    }
    // One aggregate device per kind; only the gamepad kind is served (the keyboard pad is a gamepad).
    HRESULT STDMETHODCALLTYPE CreateAggregateDevice(uint32_t kind, IGameInputDevice** out) override {
        GI_CALLED("CreateAggregateDevice");
        if (!out) return E_POINTER;
        *out = nullptr;
        constexpr uint32_t valid = GameInputKindKeyboard | GameInputKindMouse | GameInputKindGamepad |
                                   GameInputKindArcadeStick | GameInputKindFlightStick | GameInputKindRacingWheel;
        if (kind == 0 || (kind & ~valid) || (kind & (kind - 1))) return E_INVALIDARG;
        if (kind != GameInputKindGamepad) {
            not_served<15>("CreateAggregateDevice for a kind other than gamepad");
            return E_NOTIMPL;
        }
        hub().aggregate->AddRef();
        *out = hub().aggregate;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FindDeviceFromId(const AppLocalDeviceId* id, IGameInputDevice** out) override {
        GI_CALLED("FindDeviceFromId");
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!id) return E_INVALIDARG;
        Hub& h = hub();
        std::lock_guard lock(h.mu);
        auto matches = [&](Device* d) { return std::memcmp(id, &d->GetDeviceInfo()->deviceId, sizeof *id) == 0; };
        Device* found = matches(h.aggregate) ? h.aggregate : nullptr;
        for (Device* d : h.devices)
            if (!found && matches(d)) found = d;
        if (!found) return GAMEINPUT_E_DEVICE_NOT_FOUND;
        found->AddRef();
        *out = found;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FindDeviceFromObject(IUnknown*, IGameInputDevice** out) override {
        GI_UNSERVED("FindDeviceFromObject", "GAMEINPUT_E_DEVICE_NOT_FOUND");
        return not_found(out);
    }
    HRESULT STDMETHODCALLTYPE FindDeviceFromPlatformHandle(HANDLE, IGameInputDevice** out) override {
        GI_UNSERVED("FindDeviceFromPlatformHandle", "GAMEINPUT_E_DEVICE_NOT_FOUND");
        return not_found(out);
    }
    HRESULT STDMETHODCALLTYPE FindDeviceFromPlatformString(LPCWSTR, IGameInputDevice** out) override {
        GI_UNSERVED("FindDeviceFromPlatformString", "GAMEINPUT_E_DEVICE_NOT_FOUND");
        return not_found(out);
    }
    HRESULT STDMETHODCALLTYPE EnableOemDeviceSupport(uint16_t, uint16_t, uint8_t, uint8_t) override {
        GI_UNSERVED("EnableOemDeviceSupport", "E_NOTIMPL (no OEM/HID devices)");
        return E_NOTIMPL;
    }
    void STDMETHODCALLTYPE SetFocusPolicy(uint32_t) override {
        GI_UNSERVED("SetFocusPolicy", "nothing (void, ignored)");
    }

private:
    static HRESULT not_found(IGameInputDevice** out) {
        if (out) *out = nullptr;
        return GAMEINPUT_E_DEVICE_NOT_FOUND;
    }
    static HRESULT add(CbType type, IGameInputDevice* device, void* context, void* fn, GameInputCallbackToken* token) {
        if (token) *token = kInvalidCallbackToken;
        if (!fn) return E_INVALIDARG;
        Hub& h = hub();
        std::lock_guard lock(h.mu);
        Device* dev = resolve(device);
        if (device && !dev) return GAMEINPUT_E_DEVICE_NOT_FOUND;
        const GameInputCallbackToken t = h.next_token++;
        h.callbacks.push_back({t, type, context, fn, true, dev, GameInputKindUnknown, 0, 0, false});
        if (token) *token = t;
        return S_OK;
    }
    std::atomic<ULONG> refs_{1};
};

enum class Mode { None, Auto, Gc, Test, Script };

Mode mode_from_env() {
    const std::string m = script::env_lower("GTA6RT_INPUT");
    if (m == "none") return Mode::None;
    if (m == "gc") return Mode::Gc;
    if (m == "script") return Mode::Script;
    if (m == "test") return Mode::Test;
    if (!m.empty() && m != "auto")
        std::fprintf(stderr, "[rt] gameinput: GTA6RT_INPUT=%s is not none, auto, gc, script or test; using auto\n", m.c_str());
    return Mode::Auto;
}

// The object GameInputCreate serves, built on its first call; null for GTA6RT_INPUT=none.
MacInput* served() {
    static MacInput* const input = []() -> MacInput* {
        const Mode mode = mode_from_env();
        if (mode == Mode::None) return nullptr;
        Hub& h = hub();
        const std::string script_text = script::env_lower("LEONIDA_INPUT_SCRIPT");
        const std::string keyboard = script::env_lower("GTA6RT_KEYBOARD_PAD");
        const bool use_script = !script_text.empty();
        if (mode == Mode::Script && !use_script)
            std::fprintf(stderr, "[rt] gameinput: GTA6RT_INPUT=script without LEONIDA_INPUT_SCRIPT; serving live devices\n");
        const bool use_backend = mode == Mode::Gc || mode == Mode::Auto || (mode == Mode::Script && !use_script);
        mac::PadDesc agg;
        agg.key = "aggregate:gamepad";
        agg.name = "Gamepad (aggregate)";
        {
            std::lock_guard lock(h.mu);
            h.wait_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            h.worker_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            h.log_presses = script::env_lower("LEONIDA_INPUT_LOG") != "0";
            h.rumble_enabled = script::env_lower("GTA6RT_RUMBLE") != "0";
            if (const std::string polls = script::env_lower("GTA6RT_SCRIPT_MIN_POLLS"); !polls.empty()) {
                char* end = nullptr;
                const long v = std::strtol(polls.c_str(), &end, 10);
                if (end != polls.c_str() && !*end && v >= 0 && v <= 100) h.min_polls = static_cast<uint32_t>(v);
                else std::fprintf(stderr, "[rt] gameinput: GTA6RT_SCRIPT_MIN_POLLS=%s is not 0..100; using 4\n", polls.c_str());
            }
            if (keyboard == "on" || keyboard == "1") h.keyboard_mode = Hub::KeyboardMode::On;
            else if (keyboard == "off" || keyboard == "0") h.keyboard_mode = Hub::KeyboardMode::Off;
            h.aggregate = new Device(Source::Aggregate, agg.key, agg);
            h.aggregate->status = kConnectedStatus;
            if (use_script) {
                h.script = script::parse(script_text);
                mac::PadDesc d;
                d.key = "script";
                d.name = kScriptPadName;
                auto* pad = new Device(Source::Script, d.key, d);
                h.devices.push_back(pad);
                pad->handle = h.devices.size();
                pad->hw_present = true;
                h.script_pad = pad;
                h.origin_us = now_us();
                h.set_status(*pad, kConnectedStatus, h.origin_us);
                h.refresh_script(h.origin_us);
                std::fprintf(stderr,
                             "[rt] gameinput: scripted pad for LEONIDA_INPUT_SCRIPT (%zu scripted press(es), %zu "
                             "rejected)%s\n",
                             h.script.presses.size(), h.script.rejected,
                             mode == Mode::Script ? ", alone (GTA6RT_INPUT=script)"
                                                  : ", alongside the keyboard pad, the mouse and real controllers");
            }
            if (use_backend && !h.backend) h.backend = mac::make_gc_backend();
        }
        mac::Backend* backend;
        {
            std::lock_guard lock(h.mu);
            backend = h.backend.get();
        }
        if (backend) backend->start(h);
        else if (use_backend)
            std::fprintf(stderr, "[rt] gameinput: no GameController backend in this build; no input devices\n");
        std::fprintf(stderr, "[rt] gameinput: mode %s, keyboard pad %s, rumble %s\n",
                     mode == Mode::Gc ? "gc" : mode == Mode::Test ? "test" : mode == Mode::Script ? "script" : "auto",
                     h.keyboard_mode == Hub::KeyboardMode::On ? "on" : h.keyboard_mode == Hub::KeyboardMode::Off ? "off" : "auto",
                     h.rumble_enabled ? "on" : "off");
        return new MacInput;
    }();
    return input;
}

HRESULT WINAPI GameInputCreate(void** out) {
    if (!out) return E_POINTER;
    if (MacInput* input = served()) {
        input->AddRef();
        *out = static_cast<IGameInput*>(input);
        return S_OK;
    }
    g_refs.fetch_add(1);
    *out = &g_gameinput;
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "[rt] gameinput: GTA6RT_INPUT=none: no devices; E_NOTIMPL for every other slot\n");
    return S_OK;
}

}  // namespace

namespace rt::gameinput_mac {
Sink& sink() { return hub(); }
bool keyboard_pad_connected() {
    Hub& h = hub();
    std::lock_guard lock(h.mu);
    for (Device* d : h.devices)
        if (d->source == Source::Keyboard && d->status.load() != GameInputDeviceNoStatus) return true;
    return false;
}
void set_backend(std::unique_ptr<Backend> backend) {
    std::lock_guard lock(hub().mu);
    hub().backend = std::move(backend);
}
void set_rumble_observer(RumbleObserver observer) {
    std::lock_guard lock(hub().mu);
    hub().rumble_observer = observer;
}
void set_clock(ClockFn now) { g_clock.store(now ? now : &qpc_now_us); }
}  // namespace rt::gameinput_mac

SHIM_TABLE(gameinput_mac,
    {"gameinput.dll", "GameInputCreate", reinterpret_cast<void*>(&GameInputCreate)},
    // The retail executable imports this one by ordinal, so the registry key is "#1"; without the alias the loader
    // fell through to the host's gameinput.dll, which handed the title a null interface.
    {"gameinput.dll", "#1", reinterpret_cast<void*>(&GameInputCreate)},
);
