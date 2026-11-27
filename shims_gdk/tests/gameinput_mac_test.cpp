// gameinput.dll for the macOS host (src/gameinput_mac.cpp). No controller, no game data.
//
//   gameinput_mac_test                    GTA6RT_INPUT=none: the identity-only object. The retail executable
//                                         imports GameInputCreate by ordinal, and a build without SDL3 still has to
//                                         hand back an interface instead of letting the loader fall through to the
//                                         host's gameinput.dll, which answers with a null pointer the title
//                                         dereferences.
//   gameinput_mac_test --input-script     LEONIDA_INPUT_SCRIPT set: the scripted pad, found through device callbacks
//                                         and read at chosen times of a fake clock.
//   gameinput_mac_test --synthetic        GTA6RT_INPUT=test: pads and the keyboard pad driven through the hub's Sink.
//   gameinput_mac_test --census           GTA6RT_INPUT=test: every slot of IGameInput, IGameInputDevice,
//                                         IGameInputReading and the dispatcher is called, and its first call must be
//                                         logged once (an unserved one as "not served", with its failure code).
#include <windows.h>

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <mutex>
#include <tuple>
#include <vector>

#include "gameinput_abi.h"
#include "gameinput_backend.h"
#include <gta6rt/shims.h>

#include "shims.h"

using namespace gi;
namespace mac = rt::gameinput_mac;

namespace {
int g_failed = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok) ++g_failed;
}

using CreateFn = HRESULT(WINAPI*)(void**);

int identity_only(CreateFn create) {
    void* object = nullptr;
    check(create(&object) == S_OK, "GameInputCreate returns S_OK");
    check(object != nullptr, "GameInputCreate hands back a non-null interface");
    if (!object) return 1;

    auto* vtable = *static_cast<void* const* const*>(object);
    auto add_ref = reinterpret_cast<ULONG(__stdcall*)(void*)>(vtable[1]);
    auto release = reinterpret_cast<ULONG(__stdcall*)(void*)>(vtable[2]);
    check(add_ref(object) >= 1, "slot 1 (AddRef) is callable and counts");
    check(release(object) >= 0, "slot 2 (Release) is callable and does not underflow");

    // A device query answers a clean refusal through the same object rather than faulting.
    auto get_devices = reinterpret_cast<HRESULT(__stdcall*)(void*, uint32_t, void**)>(vtable[9]);
    void* devices = reinterpret_cast<void*>(0x1);
    check(get_devices(object, 0, &devices) == E_NOTIMPL, "an unserved slot returns E_NOTIMPL");
    check(devices == reinterpret_cast<void*>(0x1), "an unserved slot writes nothing to its out-parameter");

    check(create(nullptr) == E_POINTER, "a null out parameter is E_POINTER");
    return 0;
}

// ---- the call census: every slot of the four interfaces logs its first call, and an unserved one says so ----
struct StderrCapture {
    int saved = -1;
    std::string path;
    void begin() {
        std::fflush(stderr);
        const char* dir = std::getenv("TMPDIR");
        path = std::string(dir && *dir ? dir : "/tmp") + "/gameinput_census." + std::to_string(getpid()) + ".log";
        const int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        saved = dup(2);
        dup2(fd, 2);
        close(fd);
    }
    std::string end() {
        std::fflush(stderr);
        dup2(saved, 2);
        close(saved);
        std::ifstream f(path);
        std::stringstream ss;
        ss << f.rdbuf();
        std::remove(path.c_str());
        return ss.str();
    }
};

size_t count_lines(const std::string& log, const std::string& needle) {
    size_t n = 0, pos = 0;
    while ((pos = log.find(needle, pos)) != std::string::npos) ++n, pos += needle.size();
    return n;
}

struct Slot {
    const char* name;
    bool unserved;
};

const Slot kSlots[] = {
    {"GetCurrentTimestamp", false}, {"GetCurrentReading", false}, {"GetNextReading", false},
    {"GetPreviousReading", false}, {"GetTemporalReading", true}, {"RegisterReadingCallback", false},
    {"RegisterDeviceCallback", false}, {"RegisterGuideButtonCallback", false}, {"RegisterKeyboardLayoutCallback", true},
    {"StopCallback", false}, {"UnregisterCallback", false}, {"CreateDispatcher", false}, {"CreateAggregateDevice", false},
    {"FindDeviceFromId", false}, {"FindDeviceFromObject", true}, {"FindDeviceFromPlatformHandle", true},
    {"FindDeviceFromPlatformString", true}, {"EnableOemDeviceSupport", true}, {"SetFocusPolicy", true},
    {"IGameInputReading::GetInputKind", false}, {"IGameInputReading::GetSequenceNumber", false},
    {"IGameInputReading::GetTimestamp", false}, {"IGameInputReading::GetDevice", false},
    {"IGameInputReading::GetRawReport", true}, {"IGameInputReading::GetControllerAxisCount", false},
    {"IGameInputReading::GetControllerAxisState", false}, {"IGameInputReading::GetControllerButtonCount", false},
    {"IGameInputReading::GetControllerButtonState", false}, {"IGameInputReading::GetControllerSwitchCount", true},
    {"IGameInputReading::GetControllerSwitchState", true}, {"IGameInputReading::GetKeyCount", true},
    {"IGameInputReading::GetKeyState", true}, {"IGameInputReading::GetMouseState", true},
    {"IGameInputReading::GetTouchCount", true}, {"IGameInputReading::GetTouchState", true},
    {"IGameInputReading::GetMotionState", true}, {"IGameInputReading::GetArcadeStickState", true},
    {"IGameInputReading::GetFlightStickState", true}, {"IGameInputReading::GetGamepadState", false},
    {"IGameInputReading::GetRacingWheelState", true}, {"IGameInputReading::GetUiNavigationState", false},
    {"IGameInputDevice::GetDeviceInfo", false}, {"IGameInputDevice::GetDeviceStatus", false},
    {"IGameInputDevice::GetBatteryState", false}, {"IGameInputDevice::CreateForceFeedbackEffect", true},
    {"IGameInputDevice::IsForceFeedbackMotorPoweredOn", true}, {"IGameInputDevice::SetForceFeedbackMotorGain", true},
    {"IGameInputDevice::SetHapticMotorState", true}, {"IGameInputDevice::SetRumbleState", false},
    {"IGameInputDevice::SetInputSynchronizationState", true}, {"IGameInputDevice::SendInputSynchronizationHint", true},
    {"IGameInputDevice::PowerOff", true}, {"IGameInputDevice::CreateRawDeviceReport", true},
    {"IGameInputDevice::GetRawDeviceFeature", true}, {"IGameInputDevice::SetRawDeviceFeature", true},
    {"IGameInputDevice::SendRawDeviceOutput", true}, {"IGameInputDevice::SendRawDeviceOutputWithResponse", true},
    {"IGameInputDevice::ExecuteRawDeviceIoControl", true}, {"IGameInputDevice::AcquireExclusiveRawDeviceAccess", true},
    {"IGameInputDevice::ReleaseExclusiveRawDeviceAccess", true}, {"IGameInputDispatcher::Dispatch", false},
    {"IGameInputDispatcher::OpenWaitHandle", false},
};

void CALLBACK census_on_device(GameInputCallbackToken, void*, IGameInputDevice*, uint64_t, uint32_t, uint32_t) {}
void CALLBACK census_on_reading(GameInputCallbackToken, void*, IGameInputReading*, bool) {}
void CALLBACK census_on_guide(GameInputCallbackToken, void*, IGameInputDevice*, uint64_t, bool) {}
void CALLBACK census_on_layout(GameInputCallbackToken, void*, IGameInputDevice*, uint64_t, uint32_t, uint32_t) {}

void touch_every_slot(IGameInput* in, IGameInputDevice* dev, bool check_results) {
    auto ck = [&](bool ok, const char* what) {
        if (check_results) check(ok, what);
    };
    GameInputCallbackToken t1 = 0, t2 = 0, t3 = 0, t4 = 0;
    in->GetCurrentTimestamp();
    IGameInputReading* r = nullptr;
    ck(in->GetCurrentReading(GameInputKindGamepad, dev, &r) == S_OK && r, "census: a reading to walk");
    IGameInputReading* other = nullptr;
    in->GetNextReading(r, GameInputKindGamepad, dev, &other);
    if (other) other->Release();
    other = nullptr;
    in->GetPreviousReading(r, GameInputKindGamepad, dev, &other);
    if (other) other->Release();
    other = reinterpret_cast<IGameInputReading*>(0x1);
    ck(in->GetTemporalReading(1, dev, &other) == E_NOTIMPL && !other, "census: GetTemporalReading is E_NOTIMPL");
    ck(in->RegisterReadingCallback(dev, GameInputKindGamepad, 0, nullptr, census_on_reading, &t1) == S_OK, "census: RegisterReadingCallback");
    ck(in->RegisterDeviceCallback(nullptr, GameInputKindGamepad, GameInputDeviceConnected, GameInputNoEnumeration, nullptr,
                                  census_on_device, &t2) == S_OK,
       "census: RegisterDeviceCallback");
    ck(in->RegisterGuideButtonCallback(dev, nullptr, census_on_guide, &t3) == S_OK, "census: RegisterGuideButtonCallback");
    ck(in->RegisterKeyboardLayoutCallback(dev, nullptr, census_on_layout, &t4) == S_OK, "census: RegisterKeyboardLayoutCallback is accepted");
    in->StopCallback(t1);
    ck(in->UnregisterCallback(t1, 0) && in->UnregisterCallback(t2, 0) && in->UnregisterCallback(t3, 0) &&
           in->UnregisterCallback(t4, 0),
       "census: the callbacks unregister");
    IGameInputDispatcher* d = nullptr;
    ck(in->CreateDispatcher(&d) == S_OK && d, "census: CreateDispatcher");
    if (d) {
        d->Dispatch(0);
        HANDLE h = nullptr;
        if (d->OpenWaitHandle(&h) == S_OK && h) CloseHandle(h);
        d->Release();
    }
    IGameInputDevice* found = nullptr;
    ck(in->CreateAggregateDevice(GameInputKindGamepad, &found) == S_OK && found, "census: CreateAggregateDevice(gamepad)");
    if (found) found->Release();
    found = nullptr;
    const AppLocalDeviceId id = dev->GetDeviceInfo()->deviceId;
    ck(in->FindDeviceFromId(&id, &found) == S_OK && found == dev, "census: FindDeviceFromId finds the pad");
    if (found) found->Release();
    found = reinterpret_cast<IGameInputDevice*>(0x1);
    ck(in->FindDeviceFromObject(nullptr, &found) == GAMEINPUT_E_DEVICE_NOT_FOUND && !found, "census: FindDeviceFromObject is DEVICE_NOT_FOUND");
    found = reinterpret_cast<IGameInputDevice*>(0x1);
    ck(in->FindDeviceFromPlatformHandle(nullptr, &found) == GAMEINPUT_E_DEVICE_NOT_FOUND && !found, "census: FindDeviceFromPlatformHandle is DEVICE_NOT_FOUND");
    found = reinterpret_cast<IGameInputDevice*>(0x1);
    ck(in->FindDeviceFromPlatformString(nullptr, &found) == GAMEINPUT_E_DEVICE_NOT_FOUND && !found, "census: FindDeviceFromPlatformString is DEVICE_NOT_FOUND");
    ck(in->EnableOemDeviceSupport(1, 2, 3, 4) == E_NOTIMPL, "census: EnableOemDeviceSupport is E_NOTIMPL, not a false success");
    in->SetFocusPolicy(0);

    r->GetInputKind();
    r->GetSequenceNumber(0);
    r->GetTimestamp();
    IGameInputDevice* rd = nullptr;
    r->GetDevice(&rd);
    if (rd) rd->Release();
    void* raw = reinterpret_cast<void*>(0x1);
    ck(!r->GetRawReport(&raw) && !raw, "census: GetRawReport is false");
    r->GetControllerAxisCount();
    float axes[8];
    r->GetControllerAxisState(8, axes);
    r->GetControllerButtonCount();
    bool buttons[16];
    r->GetControllerButtonState(16, buttons);
    GameInputSwitchPosition sw[2];
    ck(r->GetControllerSwitchCount() == 0 && r->GetControllerSwitchState(2, sw) == 0, "census: no switches");
    ck(r->GetKeyCount() == 0 && r->GetKeyState(1, nullptr) == 0, "census: no keys");
    ck(!r->GetMouseState(nullptr), "census: no mouse");
    ck(r->GetTouchCount() == 0 && r->GetTouchState(1, nullptr) == 0, "census: no touch");
    ck(!r->GetMotionState(nullptr) && !r->GetArcadeStickState(nullptr) && !r->GetFlightStickState(nullptr) &&
           !r->GetRacingWheelState(nullptr),
       "census: no motion, arcade, flight or wheel state");
    GameInputGamepadState gp{};
    r->GetGamepadState(&gp);
    GameInputUiNavigationState ui{};
    r->GetUiNavigationState(&ui);
    r->Release();

    dev->GetDeviceInfo();
    dev->GetDeviceStatus();
    GameInputBatteryState battery{};
    dev->GetBatteryState(&battery);
    void* out = reinterpret_cast<void*>(0x1);
    ck(dev->CreateForceFeedbackEffect(0, nullptr, &out) == E_INVALIDARG && !out, "census: CreateForceFeedbackEffect is E_INVALIDARG");
    ck(!dev->IsForceFeedbackMotorPoweredOn(0), "census: force-feedback motor is not powered");
    dev->SetForceFeedbackMotorGain(0, 1.0f);
    ck(dev->SetHapticMotorState(0, nullptr) == E_INVALIDARG, "census: SetHapticMotorState is E_INVALIDARG");
    GameInputRumbleParams rumble{0.25f, 0.5f, 0, 0};
    dev->SetRumbleState(&rumble);
    dev->SetInputSynchronizationState(true);
    dev->SendInputSynchronizationHint();
    dev->PowerOff();
    out = reinterpret_cast<void*>(0x1);
    ck(dev->CreateRawDeviceReport(0, 0, &out) == E_NOTIMPL && !out, "census: CreateRawDeviceReport is E_NOTIMPL");
    out = reinterpret_cast<void*>(0x1);
    ck(dev->GetRawDeviceFeature(0, &out) == E_NOTIMPL && !out, "census: GetRawDeviceFeature is E_NOTIMPL");
    ck(dev->SetRawDeviceFeature(nullptr) == E_NOTIMPL, "census: SetRawDeviceFeature is E_NOTIMPL");
    ck(dev->SendRawDeviceOutput(nullptr) == E_NOTIMPL, "census: SendRawDeviceOutput is E_NOTIMPL");
    out = reinterpret_cast<void*>(0x1);
    ck(dev->SendRawDeviceOutputWithResponse(nullptr, &out) == E_NOTIMPL && !out, "census: SendRawDeviceOutputWithResponse is E_NOTIMPL");
    size_t size = 9;
    ck(dev->ExecuteRawDeviceIoControl(0, 0, nullptr, 0, nullptr, &size) == E_NOTIMPL && size == 0, "census: ExecuteRawDeviceIoControl is E_NOTIMPL");
    ck(!dev->AcquireExclusiveRawDeviceAccess(0), "census: exclusive raw access is refused");
    dev->ReleaseExclusiveRawDeviceAccess();
}

int census(CreateFn create) {
    SetEnvironmentVariableA("GTA6RT_INPUT", "test");
    SetEnvironmentVariableA("GTA6RT_KEYBOARD_PAD", nullptr);
    StderrCapture cap;
    cap.begin();
    IGameInput* input = nullptr;
    check(create(reinterpret_cast<void**>(&input)) == S_OK && input, "census: GameInputCreate returns an instance");
    if (!input) return 1;
    mac::PadDesc desc;
    desc.key = "test:census";
    desc.name = "Census pad";
    desc.rumble_motors = mac::kRumbleLow | mac::kRumbleHigh;
    const uint64_t handle = mac::sink().connect(desc);
    mac::sink().state(handle, mac::PadState{}, false);
    IGameInputReading* r = nullptr;
    check(input->GetCurrentReading(GameInputKindGamepad, nullptr, &r) == S_OK && r, "census: the pad has a reading");
    IGameInputDevice* dev = nullptr;
    if (r) r->GetDevice(&dev);
    if (r) r->Release();
    check(dev != nullptr, "census: a device to call");
    if (!dev) return 1;

    touch_every_slot(input, dev, false);
    const std::string first = cap.end();
    size_t missing = 0, wrong = 0;
    for (const Slot& s : kSlots) {
        const std::string head = std::string("first call of ") + s.name;
        const size_t served = count_lines(first, head + "\n");
        const size_t unserved = count_lines(first, head + ": not served, returns ");
        if (served + unserved != 1) {
            std::printf("     census: %s logged %zu+%zu times\n", s.name, served, unserved);
            ++missing;
        } else if ((unserved == 1) != s.unserved) {
            std::printf("     census: %s logged as %s\n", s.name, unserved ? "unserved" : "served");
            ++wrong;
        }
    }
    check(missing == 0, "census: every one of the 62 slots logs its first call exactly once");
    check(wrong == 0, "census: unserved slots say 'not served' and the served ones do not");
    size_t unserved_slots = 0;
    for (const Slot& s : kSlots) unserved_slots += s.unserved;
    check(count_lines(first, ": not served, returns ") == unserved_slots, "census: no other unserved line");
    std::printf("     census: %zu slots, %zu unserved\n", sizeof(kSlots) / sizeof(kSlots[0]), unserved_slots);

    cap.begin();
    touch_every_slot(input, dev, true);
    const std::string second = cap.end();
    check(second.find("first call of") == std::string::npos, "census: a second round logs nothing new (once per slot)");

    input->Release();
    return 0;
}

// ---- the scripted pad ----

uint64_t g_now_us = 7'000'000;  // the fake clock; the script's clock starts at GameInputCreate
uint64_t fake_clock() { return g_now_us; }
void at(double seconds) { g_now_us = 7'000'000 + static_cast<uint64_t>(seconds * 1e6 + 0.5); }

struct Seen {
    int calls = 0;
    IGameInputDevice* device = nullptr;
    uint32_t current = 0, previous = 0xFFFFFFFF;
    DWORD thread = 0;
    HANDLE done = nullptr;
};
void CALLBACK on_device(GameInputCallbackToken, void* context, IGameInputDevice* device, uint64_t, uint32_t current,
                        uint32_t previous) {
    auto* seen = static_cast<Seen*>(context);
    ++seen->calls;
    seen->device = device;
    seen->current = current;
    seen->previous = previous;
    seen->thread = GetCurrentThreadId();
    if (seen->done) SetEvent(seen->done);
}

struct ReadingSeen {
    std::atomic<int> calls{0};
    DWORD thread = 0;
    HANDLE done = nullptr;
};
void CALLBACK on_reading(GameInputCallbackToken, void* context, IGameInputReading* reading, bool) {
    auto* seen = static_cast<ReadingSeen*>(context);
    GameInputGamepadState s{};
    reading->GetGamepadState(&s);
    seen->thread = GetCurrentThreadId();
    if (++seen->calls >= 4 && seen->done) SetEvent(seen->done);
}

struct Pad {
    uint32_t buttons = 0xFFFFFFFF;
    float right_y = -2;
    uint32_t ui = 0xFFFFFFFF;
    uint64_t seq = 0;
    bool a_button = false;
};
Pad read_at(IGameInput* input, IGameInputDevice* device, double seconds) {
    at(seconds);
    Pad p;
    IGameInputReading* r = nullptr;
    if (input->GetCurrentReading(GameInputKindGamepad, device, &r) != S_OK || !r) return p;
    GameInputGamepadState s{};
    if (r->GetGamepadState(&s)) p.buttons = s.buttons, p.right_y = s.rightThumbstickY;
    GameInputUiNavigationState u{};
    if (r->GetUiNavigationState(&u)) p.ui = u.buttons;
    p.seq = r->GetSequenceNumber(GameInputKindGamepad);
    bool buttons[15] = {};
    if (r->GetControllerButtonState(15, buttons) == 15) p.a_button = buttons[0];
    r->Release();
    return p;
}

int scripted_pad(CreateFn create) {
    // Four presses (two overlapping A presses, Menu, a right-stick throw) and three rejected entries.
    SetEnvironmentVariableA("LEONIDA_INPUT_SCRIPT", "a@1, START@2.5 ,a@1.2,rsup@4,bogus@1,a@-1,");
    rt::gameinput_mac::set_clock(fake_clock);
    at(0);

    IGameInput* input = nullptr;
    check(create(reinterpret_cast<void**>(&input)) == S_OK && input, "script: GameInputCreate returns an instance");
    if (!input) return 1;
    IUnknown* unk = nullptr;
    check(input->QueryInterface(IID_IGameInput, reinterpret_cast<void**>(&unk)) == S_OK && unk == input,
          "script: QueryInterface(IID_IGameInput) is the same object");
    if (unk) unk->Release();
    check(input->GetCurrentTimestamp() == g_now_us, "script: GetCurrentTimestamp reads the shim's clock");

    // Device callbacks: a blocking enumeration reports the pad before it returns, on this thread.
    Seen blocking;
    GameInputCallbackToken token = 0;
    check(input->RegisterDeviceCallback(nullptr, GameInputKindGamepad, GameInputDeviceConnected,
                                        GameInputBlockingEnumeration, &blocking, on_device, &token) == S_OK &&
              token != kInvalidCallbackToken,
          "script: RegisterDeviceCallback (blocking) is served");
    check(blocking.calls == 1 && blocking.device && blocking.thread == GetCurrentThreadId(),
          "script: blocking enumeration reports one device on the caller's thread");
    check((blocking.current & GameInputDeviceConnected) && blocking.previous == GameInputDeviceNoStatus,
          "script: the device is reported connected, from no status");
    IGameInputDevice* device = blocking.device;
    if (!device) return 1;
    const GameInputDeviceInfo* info = device->GetDeviceInfo();
    check(info && info->deviceFamily == GameInputFamilyXboxOne && (info->supportedInput & GameInputKindGamepad),
          "script: the device is an Xbox One gamepad");
    check(info && info->gamepadInfo && info->gamepadInfo->aButtonLabel == GameInputLabelXboxA,
          "script: the A button carries the Xbox A label");
    check(info && info->displayName && info->displayName->sizeInBytes > 0, "script: the device has a display name");
    check(device->GetDeviceStatus() & GameInputDeviceConnected, "script: GetDeviceStatus says connected");
    IGameInputDevice* found = nullptr;
    check(info && input->FindDeviceFromId(&info->deviceId, &found) == S_OK && found == device,
          "script: FindDeviceFromId finds the pad");
    if (found) found->Release();
    check(input->UnregisterCallback(token, 0), "script: the blocking callback unregisters");

    // No enumeration, or a status filter the pad does not match: nothing is reported.
    Seen quiet;
    check(input->RegisterDeviceCallback(nullptr, GameInputKindGamepad, GameInputDeviceConnected, GameInputNoEnumeration,
                                        &quiet, on_device, &token) == S_OK,
          "script: RegisterDeviceCallback without enumeration is served");
    input->UnregisterCallback(token, 0);
    check(input->RegisterDeviceCallback(nullptr, GameInputKindGamepad, GameInputDeviceWireless,
                                        GameInputBlockingEnumeration, &quiet, on_device, &token) == S_OK,
          "script: RegisterDeviceCallback with another status filter is served");
    input->UnregisterCallback(token, 0);
    check(input->RegisterDeviceCallback(nullptr, GameInputKindKeyboard, GameInputDeviceAnyStatus,
                                        GameInputBlockingEnumeration, &quiet, on_device, &token) == S_OK,
          "script: RegisterDeviceCallback for keyboards is served");
    input->UnregisterCallback(token, 0);
    check(quiet.calls == 0, "script: no report without enumeration, for another status or another kind");

    // Async enumeration: reported after the call, from another thread.
    Seen async;
    async.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    check(input->RegisterDeviceCallback(nullptr, GameInputKindUnknown, GameInputDeviceAnyStatus,
                                        GameInputAsyncEnumeration, &async, on_device, &token) == S_OK,
          "script: RegisterDeviceCallback (async) is served");
    check(WaitForSingleObject(async.done, 5000) == WAIT_OBJECT_0 && async.calls == 1 && async.device == device,
          "script: async enumeration reports the pad");
    check(async.thread != GetCurrentThreadId(), "script: async enumeration reports it from another thread");
    input->UnregisterCallback(token, 0);

    // With a dispatcher, the title's Dispatch delivers the report on its own thread.
    IGameInputDispatcher* dispatcher = nullptr;
    check(input->CreateDispatcher(&dispatcher) == S_OK && dispatcher, "script: CreateDispatcher is served");
    if (dispatcher) {
        HANDLE wait = nullptr;
        check(dispatcher->OpenWaitHandle(&wait) == S_OK && wait, "script: the dispatcher has a wait handle");
        Seen dispatched;
        check(input->RegisterDeviceCallback(nullptr, GameInputKindGamepad, GameInputDeviceConnected,
                                            GameInputAsyncEnumeration, &dispatched, on_device, &token) == S_OK,
              "script: RegisterDeviceCallback (async, with a dispatcher) is served");
        check(wait && WaitForSingleObject(wait, 5000) == WAIT_OBJECT_0, "script: the wait handle is signalled");
        check(dispatched.calls == 0, "script: nothing is delivered before Dispatch");
        check(!dispatcher->Dispatch(0) && dispatched.calls == 1 && dispatched.thread == GetCurrentThreadId(),
              "script: Dispatch delivers the report on the caller's thread");
        input->UnregisterCallback(token, 0);
        if (wait) CloseHandle(wait);
        dispatcher->Release();
    }

    GameInputCallbackToken reading_token = 0;
    ReadingSeen reading_seen;
    reading_seen.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    check(input->RegisterReadingCallback(device, GameInputKindGamepad, 0, &reading_seen, on_reading, &reading_token) ==
                  S_OK &&
              reading_token != kInvalidCallbackToken,
          "script: reading callbacks are served");

    // Readings follow the script's timeline: each press is held for 0.3 s, then released.
    const Pad start = read_at(input, nullptr, 0.0);
    check(start.buttons == 0 && start.right_y == 0, "script: nothing is held at 0 s");
    const Pad idle = read_at(input, device, 0.9);
    check(idle.buttons == 0 && idle.seq == start.seq, "script: no new reading while nothing changes");
    const Pad a = read_at(input, device, 1.0);
    check(a.buttons == GameInputGamepadA && a.a_button && a.seq > idle.seq, "script: A is held at 1.0 s");
    check(a.ui == GameInputUiNavigationAccept, "script: A reads as UI Accept");
    check(read_at(input, device, 1.25).buttons == GameInputGamepadA, "script: overlapping A presses hold A at 1.25 s");
    check(read_at(input, device, 1.45).buttons == GameInputGamepadA,
          "script: a later press of the same button is not cut by the earlier one's release");
    const Pad released = read_at(input, device, 1.55);
    check(released.buttons == 0 && !released.a_button, "script: A is released after its last 0.3 s hold");
    const Pad menu = read_at(input, device, 2.6);
    check(menu.buttons == GameInputGamepadMenu && menu.ui == GameInputUiNavigationMenu,
          "script: START (any case) holds Menu at 2.6 s");
    check(read_at(input, device, 2.85).buttons == 0, "script: Menu is released at 2.85 s");
    const Pad up = read_at(input, device, 4.1);
    check(up.buttons == 0 && up.right_y == 1.0f && up.ui == GameInputUiNavigationScrollUp,
          "script: rsup throws the right stick up at 4.1 s");
    check(read_at(input, device, 4.5).right_y == 0, "script: the stick is back at rest at 4.5 s");
    check(read_at(input, device, 60).buttons == 0, "script: nothing is held after the script ends");

    // The history: the reading before the current one is the stick throw, and the one after that throw is the rest.
    IGameInputReading* now = nullptr;
    IGameInputReading* before = nullptr;
    IGameInputReading* after = nullptr;
    check(input->GetCurrentReading(GameInputKindUnknown, nullptr, &now) == S_OK && now, "script: a current reading");
    if (now) {
        GameInputGamepadState s{};
        check(input->GetPreviousReading(now, GameInputKindGamepad, device, &before) == S_OK && before &&
                  before->GetGamepadState(&s) && s.rightThumbstickY == 1.0f,
              "script: GetPreviousReading steps back to the stick throw");
        if (before) {
            check(input->GetNextReading(before, GameInputKindGamepad, nullptr, &after) == S_OK && after == now,
                  "script: GetNextReading steps forward to the current reading");
            before->Release();
        }
        if (after) after->Release();
        IGameInputReading* none = nullptr;
        check(input->GetNextReading(now, GameInputKindUnknown, nullptr, &none) == GAMEINPUT_E_READING_NOT_FOUND &&
                  !none,
              "script: nothing after the current reading");
        IGameInputDevice* of = nullptr;
        now->GetDevice(&of);
        check(of == device, "script: a reading belongs to the pad");
        if (of) of->Release();
        now->Release();
    }

    IGameInputReading* motion = nullptr;
    check(input->GetCurrentReading(GameInputKindMotion, nullptr, &motion) == GAMEINPUT_E_READING_NOT_FOUND && !motion,
          "script: the pad has no motion readings");
    check(WaitForSingleObject(reading_seen.done, 5000) == WAIT_OBJECT_0 && reading_seen.calls >= 4,
          "script: the reading callback heard the script's changes (A, release, Menu, release, stick ...)");
    check(reading_seen.thread != GetCurrentThreadId(), "script: reading callbacks run on a thread of the shim");
    input->UnregisterCallback(reading_token, 0);
    check(input->RegisterReadingCallback(device, GameInputKindGamepad, 0, nullptr, nullptr, &reading_token) ==
              E_INVALIDARG,
          "script: a reading callback without a function is E_INVALIDARG");
    void* second = nullptr;
    check(create(&second) == S_OK && second == input, "script: a second GameInputCreate returns the same instance");
    if (second) static_cast<IGameInput*>(second)->Release();
    input->Release();
    return 0;
}

// ---- synthetic devices through the hub's Sink (GTA6RT_INPUT=test) ----


struct DeviceEvents {
    std::mutex mu;
    std::vector<std::tuple<IGameInputDevice*, uint32_t, uint32_t, DWORD>> list;
    size_t count() {
        std::lock_guard l(mu);
        return list.size();
    }
};
void CALLBACK on_device_event(GameInputCallbackToken, void* context, IGameInputDevice* device, uint64_t, uint32_t current,
                              uint32_t previous) {
    auto* e = static_cast<DeviceEvents*>(context);
    std::lock_guard l(e->mu);
    e->list.emplace_back(device, current, previous, GetCurrentThreadId());
}

struct ReadingEvents {
    std::atomic<int> calls{0};
    std::atomic<uint32_t> last_buttons{0};
    std::atomic<bool> overrun{false};
};
void CALLBACK on_reading_event(GameInputCallbackToken, void* context, IGameInputReading* reading, bool overrun) {
    auto* e = static_cast<ReadingEvents*>(context);
    GameInputGamepadState s{};
    reading->GetGamepadState(&s);
    e->last_buttons = s.buttons;
    if (overrun) e->overrun = true;
    ++e->calls;
}

struct GuideEvents {
    std::atomic<int> calls{0};
    std::atomic<bool> pressed{false};
};
void CALLBACK on_guide_event(GameInputCallbackToken, void* context, IGameInputDevice*, uint64_t, bool pressed) {
    auto* e = static_cast<GuideEvents*>(context);
    e->pressed = pressed;
    ++e->calls;
}

template <class F>
bool wait_for(F pred, int ms = 3000) {
    for (int i = 0; i < ms; ++i) {
        if (pred()) return true;
        Sleep(1);
    }
    return pred();
}

std::atomic<int> g_rumble_calls{0};
mac::RumbleLevels g_rumble_last;
uint64_t g_rumble_handle = 0;
void rumble_seen(uint64_t handle, const mac::RumbleLevels& l) {
    g_rumble_handle = handle;
    g_rumble_last = l;
    ++g_rumble_calls;
}

HRESULT current_state(IGameInput* in, IGameInputDevice* dev, GameInputGamepadState* s, IGameInputDevice** from = nullptr) {
    IGameInputReading* r = nullptr;
    const HRESULT hr = in->GetCurrentReading(GameInputKindGamepad, dev, &r);
    if (hr != S_OK) return hr;
    r->GetGamepadState(s);
    if (from) r->GetDevice(from);
    r->Release();
    return hr;
}

mac::PadState pad_state(uint32_t buttons, float lx = 0, float ry = 0, float rt = 0) {
    mac::PadState s;
    s.buttons = buttons;
    s.lx = lx;
    s.ry = ry;
    s.rt = rt;
    return s;
}

int synthetic(CreateFn create) {
    SetEnvironmentVariableA("GTA6RT_INPUT", "test");
    SetEnvironmentVariableA("GTA6RT_KEYBOARD_PAD", nullptr);
    mac::set_rumble_observer(rumble_seen);
    IGameInput* input = nullptr;
    check(create(reinterpret_cast<void**>(&input)) == S_OK && input, "synthetic: GameInputCreate returns an instance");
    if (!input) return 1;
    mac::Sink& sink = mac::sink();

    IGameInputReading* none = nullptr;
    check(input->GetCurrentReading(GameInputKindGamepad, nullptr, &none) == GAMEINPUT_E_READING_NOT_FOUND && !none,
          "synthetic: with no device there is no reading");

    DeviceEvents events;
    GameInputCallbackToken device_token = 0;
    check(input->RegisterDeviceCallback(nullptr, GameInputKindGamepad, GameInputDeviceConnected, GameInputAsyncEnumeration,
                                        &events, on_device_event, &device_token) == S_OK,
          "synthetic: RegisterDeviceCallback (async) with no device yet");
    Sleep(20);
    check(events.count() == 0, "synthetic: nothing to enumerate yet");

    mac::PadDesc pad_desc;
    pad_desc.key = "test:pad";
    pad_desc.name = "Synthetic pad";
    pad_desc.vendor = 0x045E;
    pad_desc.product = 0x0B12;
    pad_desc.rumble_motors = mac::kRumbleLow | mac::kRumbleHigh | mac::kRumbleLeftTrigger | mac::kRumbleRightTrigger;
    const uint64_t pad1 = sink.connect(pad_desc);
    check(pad1 != 0, "synthetic: Sink::connect returns a handle");
    check(wait_for([&] { return events.count() == 1; }), "synthetic: connecting a pad reports it to the device callback");
    IGameInputDevice* device = nullptr;
    {
        std::lock_guard l(events.mu);
        device = std::get<0>(events.list[0]);
        check((std::get<1>(events.list[0]) & GameInputDeviceConnected) && std::get<2>(events.list[0]) == 0,
              "synthetic: the report is connected, from no status");
        check(std::get<3>(events.list[0]) != GetCurrentThreadId(), "synthetic: the report runs on a thread of the shim");
    }
    const GameInputDeviceInfo* info = device->GetDeviceInfo();
    check(info->vendorId == 0x045E && info->productId == 0x0B12 && info->deviceFamily == GameInputFamilyXboxOne &&
              info->supportedRumbleMotors == 15,
          "synthetic: device info carries the ids, the Xbox One family and the rumble motors");
    check(info->displayName && std::strcmp(info->displayName->data, "Synthetic pad") == 0, "synthetic: display name");

    GameInputGamepadState s{};
    check(current_state(input, device, &s) == S_OK && s.buttons == 0, "synthetic: a connected pad has an initial reading");

    sink.state(pad1, pad_state(mac::btn::A | mac::btn::RightShoulder, 0.5f, -0.25f, 0.75f), false);
    IGameInputDevice* from = nullptr;
    check(current_state(input, nullptr, &s, &from) == S_OK && s.buttons == (GameInputGamepadA | GameInputGamepadRightShoulder) &&
              s.leftThumbstickX == 0.5f && s.rightThumbstickY == -0.25f && s.rightTrigger == 0.75f && from == device,
          "synthetic: buttons, sticks and triggers reach GetGamepadState (no device filter)");
    if (from) from->Release();
    IGameInputReading* reading = nullptr;
    input->GetCurrentReading(GameInputKindUnknown, device, &reading);
    if (reading) {
        GameInputUiNavigationState ui{};
        reading->GetUiNavigationState(&ui);
        check(ui.buttons == (GameInputUiNavigationAccept | GameInputUiNavigationPageRight | GameInputUiNavigationPageDown),
              "synthetic: UI navigation follows the pad");
        float axes[6] = {};
        bool buttons[15] = {};
        check(reading->GetControllerAxisState(6, axes) == 6 && axes[0] == 0.75f && axes[3] == 0.375f && axes[5] == 0.75f,
              "synthetic: controller axes are 0..1 with 0.5 at rest");
        check(reading->GetControllerButtonState(15, buttons) == 15 && buttons[0] && buttons[10] && !buttons[1],
              "synthetic: controller buttons are in SDL order");
        reading->Release();
    }
    sink.state(pad1, pad_state(mac::btn::LeftThumb | mac::btn::RightThumb), true);
    input->GetCurrentReading(GameInputKindUnknown, device, &reading);
    if (reading) {
        bool buttons[15] = {};
        reading->GetControllerButtonState(15, buttons);
        check(buttons[5] && buttons[7] && buttons[8], "synthetic: guide and the stick clicks are controller buttons");
        reading->Release();
    }

    // Reading callbacks: every change of the device, or past an analog threshold.
    sink.state(pad1, pad_state(0), false);
    ReadingEvents all, thresholded;
    GameInputCallbackToken t_all = 0, t_thr = 0;
    check(input->RegisterReadingCallback(device, GameInputKindGamepad, 0, &all, on_reading_event, &t_all) == S_OK &&
              input->RegisterReadingCallback(nullptr, GameInputKindGamepad, 0.5f, &thresholded, on_reading_event, &t_thr) == S_OK,
          "synthetic: reading callbacks register");
    sink.state(pad1, pad_state(0, 0.1f), false);
    sink.state(pad1, pad_state(0, 0.2f), false);
    sink.state(pad1, pad_state(0, 0.9f), false);
    sink.state(pad1, pad_state(mac::btn::B, 0.9f), false);
    check(wait_for([&] { return all.calls == 4; }), "synthetic: the reading callback hears all four changes");
    check(wait_for([&] { return thresholded.calls == 2; }) && thresholded.last_buttons == GameInputGamepadB,
          "synthetic: a 0.5 threshold lets a button change and the big stick move through, not the small ones");
    sink.state(pad1, pad_state(mac::btn::B, 0.9f), false);
    Sleep(30);
    check(all.calls == 4, "synthetic: an unchanged state is not a new reading");
    input->UnregisterCallback(t_all, 0);
    input->UnregisterCallback(t_thr, 0);

    GuideEvents guide;
    GameInputCallbackToken t_guide = 0;
    check(input->RegisterGuideButtonCallback(nullptr, &guide, on_guide_event, &t_guide) == S_OK, "synthetic: guide callback");
    sink.state(pad1, pad_state(0), true);
    check(wait_for([&] { return guide.calls == 1; }) && guide.pressed, "synthetic: the guide button press is reported");
    sink.state(pad1, pad_state(0), false);
    check(wait_for([&] { return guide.calls == 2; }) && !guide.pressed, "synthetic: and its release");
    input->UnregisterCallback(t_guide, 0);

    // History: the last 256 readings, and a step across a dropped reading is refused.
    IGameInputReading* first = nullptr;
    input->GetCurrentReading(GameInputKindGamepad, device, &first);
    for (int i = 1; i <= 300; ++i) sink.state(pad1, pad_state(0, 0, static_cast<float>(i) / 1000.0f), false);
    IGameInputReading* cursor = nullptr;
    input->GetCurrentReading(GameInputKindGamepad, device, &cursor);
    int back = 0;
    HRESULT hr = S_OK;
    while (cursor) {
        IGameInputReading* prev = nullptr;
        hr = input->GetPreviousReading(cursor, GameInputKindGamepad, device, &prev);
        cursor->Release();
        cursor = prev;
        if (hr != S_OK) break;
        ++back;
    }
    check(back == 255 && hr == GAMEINPUT_E_READING_NOT_FOUND, "synthetic: the history holds 256 readings (255 steps back)");
    IGameInputReading* step = nullptr;
    check(first && input->GetNextReading(first, GameInputKindGamepad, device, &step) ==
                       GAMEINPUT_E_REFERENCE_READING_TOO_OLD && !step,
          "synthetic: stepping from a reading that left the history is REFERENCE_READING_TOO_OLD");
    GameInputGamepadState held{};
    check(first && first->GetGamepadState(&held) && held.rightThumbstickY == 0.0f,
          "synthetic: a reading the title holds stays valid after it left the history");
    if (first) first->Release();

    // A second pad (twin) and the keyboard pad, which stands in only while no pad is attached.
    pad_desc.name = "Synthetic pad 2";
    const uint64_t pad2 = sink.connect(pad_desc);
    IGameInputDevice* device2 = nullptr;
    check(wait_for([&] { return events.count() == 2; }), "synthetic: a twin pad is a second connection");
    {
        std::lock_guard l(events.mu);
        device2 = std::get<0>(events.list[1]);
    }
    check(device2 && device2 != device, "synthetic: a twin pad gets its own device object");
    check(info && device2 && std::memcmp(&info->deviceId, &device2->GetDeviceInfo()->deviceId, 32) != 0,
          "synthetic: and its own device id");
    mac::PadDesc kb;
    kb.key = "test:keyboard";
    kb.name = "Keyboard pad (test)";
    kb.kind = mac::PadKind::Keyboard;
    const uint64_t keyboard = sink.connect(kb);
    Sleep(30);
    check(events.count() == 2, "synthetic: the keyboard pad stays disconnected while pads are attached");
    sink.state(keyboard, pad_state(mac::btn::Y), false);
    sink.state(pad2, pad_state(mac::btn::X), false);
    check(current_state(input, nullptr, &s) == S_OK && s.buttons == GameInputGamepadX,
          "synthetic: with no device given the newest reading across devices wins, and the keyboard's is ignored");

    IGameInputDevice* aggregate = nullptr;
    check(input->CreateAggregateDevice(GameInputKindGamepad, &aggregate) == S_OK && aggregate &&
              aggregate->GetDeviceInfo()->deviceFamily == GameInputFamilyAggregate,
          "synthetic: CreateAggregateDevice(gamepad) is served");
    IGameInputDevice* ag2 = nullptr;
    check(input->CreateAggregateDevice(GameInputKindKeyboard, &ag2) == E_NOTIMPL && !ag2,
          "synthetic: a keyboard aggregate is not served (E_NOTIMPL)");
    check(input->CreateAggregateDevice(0, &ag2) == E_INVALIDARG && input->CreateAggregateDevice(GameInputKindGamepad | GameInputKindMouse, &ag2) == E_INVALIDARG,
          "synthetic: no kind or several kinds are E_INVALIDARG");
    sink.state(pad1, pad_state(mac::btn::Menu), false);
    check(current_state(input, aggregate, &s, &from) == S_OK && s.buttons == GameInputGamepadMenu && from == device,
          "synthetic: an aggregate's current reading is the newest of any pad");
    if (from) from->Release();
    IGameInputReading* ref = nullptr;
    input->GetCurrentReading(GameInputKindGamepad, device2, &ref);
    IGameInputReading* next = nullptr;
    check(ref && input->GetNextReading(ref, GameInputKindGamepad, aggregate, &next) == S_OK && next,
          "synthetic: GetNextReading through an aggregate steps across devices");
    if (next) next->Release();
    if (ref) ref->Release();
    IGameInputDevice* found = nullptr;
    check(input->FindDeviceFromId(&aggregate->GetDeviceInfo()->deviceId, &found) == S_OK && found == aggregate,
          "synthetic: FindDeviceFromId finds the aggregate");
    if (found) found->Release();

    // Rumble reaches the backend seam, clamped and without repeats.
    GameInputRumbleParams rumble{0.5f, 2.0f, -1.0f, 0.25f};
    device->SetRumbleState(&rumble);
    device->SetRumbleState(&rumble);
    check(g_rumble_calls == 1 && g_rumble_handle == pad1 && g_rumble_last.low == 0.5f && g_rumble_last.high == 1.0f &&
              g_rumble_last.left_trigger == 0.0f && g_rumble_last.right_trigger == 0.25f,
          "synthetic: SetRumbleState is clamped to 0..1 and a repeated state is not sent twice");
    rumble = {0, 0, 0, 0};
    device->SetRumbleState(&rumble);
    check(g_rumble_calls == 2 && g_rumble_last.low == 0.0f, "synthetic: rumble off is sent");
    GameInputBatteryState battery{};
    device->GetBatteryState(&battery);
    check(battery.status == GameInputBatteryUnknown, "synthetic: battery is unknown without a backend");

    // Disconnect: the keyboard pad takes over only when the last pad goes.
    sink.disconnect(pad2);
    check(wait_for([&] { return events.count() == 3; }), "synthetic: a disconnect is reported");
    {
        std::lock_guard l(events.mu);
        check(std::get<0>(events.list[2]) == device2 && std::get<1>(events.list[2]) == GameInputDeviceNoStatus &&
                  (std::get<2>(events.list[2]) & GameInputDeviceConnected),
              "synthetic: reported as no status, from connected");
    }
    check(device2->GetDeviceStatus() == GameInputDeviceNoStatus, "synthetic: its status is no status");
    IGameInputReading* gone = nullptr;
    check(input->GetCurrentReading(GameInputKindGamepad, device2, &gone) == GAMEINPUT_E_DEVICE_DISCONNECTED && !gone,
          "synthetic: a disconnected pad has no readings (GAMEINPUT_E_DEVICE_DISCONNECTED)");
    check(current_state(input, nullptr, &s) == S_OK && s.buttons == GameInputGamepadMenu,
          "synthetic: readings without a device come from the pads that remain");
    sink.disconnect(pad1);
    check(wait_for([&] { return events.count() == 5; }), "synthetic: the last pad leaving connects the keyboard pad");
    IGameInputDevice* kbd = nullptr;
    {
        std::lock_guard l(events.mu);
        kbd = std::get<0>(events.list[4]);
        check(std::get<1>(events.list[4]) & GameInputDeviceConnected, "synthetic: the keyboard pad is reported connected");
    }
    sink.state(keyboard, pad_state(mac::btn::Y), false);
    check(current_state(input, nullptr, &s, &from) == S_OK && s.buttons == GameInputGamepadY && from == kbd,
          "synthetic: the keyboard pad's state is the current reading");
    if (from) from->Release();

    const uint64_t again = sink.connect(pad_desc);
    check(again == pad1 || again == pad2, "synthetic: the same key connecting again gets its device back");
    check(wait_for([&] { return events.count() >= 7; }), "synthetic: the pad's return and the keyboard pad's exit are reported");
    check(kbd->GetDeviceStatus() == GameInputDeviceNoStatus, "synthetic: the keyboard pad steps aside again");
    IGameInputDevice* found_again = nullptr;
    check(input->FindDeviceFromId(&info->deviceId, &found_again) == S_OK && (found_again == device || found_again == device2),
          "synthetic: FindDeviceFromId still finds it");
    if (found_again) found_again->Release();
    input->UnregisterCallback(device_token, 0);

    // A dispatcher takes over the delivery from the shim's thread.
    IGameInputDispatcher* dispatcher = nullptr;
    check(input->CreateDispatcher(&dispatcher) == S_OK && dispatcher, "synthetic: CreateDispatcher");
    DeviceEvents queued;
    GameInputCallbackToken q_token = 0;
    input->RegisterDeviceCallback(nullptr, GameInputKindGamepad, GameInputDeviceAnyStatus, GameInputNoEnumeration, &queued,
                                  on_device_event, &q_token);
    sink.disconnect(again);
    Sleep(30);
    check(queued.count() == 0, "synthetic: with a dispatcher, events wait for Dispatch");
    dispatcher->Dispatch(0);
    check(queued.count() >= 1 && std::get<3>(queued.list[0]) == GetCurrentThreadId(),
          "synthetic: Dispatch runs them on the caller's thread");
    input->UnregisterCallback(q_token, 0);
    dispatcher->Release();

    // Cost of the per-frame call: GetCurrentReading + GetGamepadState + Release, one pad connected (printed, not asserted).
    const uint64_t bench_pad = sink.connect(pad_desc);
    sink.state(bench_pad, pad_state(mac::btn::A, 0.5f), false);
    LARGE_INTEGER f0, c0, c1;
    QueryPerformanceFrequency(&f0);
    constexpr int kIters = 200000;
    QueryPerformanceCounter(&c0);
    GameInputGamepadState bs{};
    for (int i = 0; i < kIters; ++i) {
        IGameInputReading* r = nullptr;
        input->GetCurrentReading(GameInputKindGamepad, nullptr, &r);
        r->GetGamepadState(&bs);
        r->Release();
    }
    QueryPerformanceCounter(&c1);
    std::printf("bench: GetCurrentReading+GetGamepadState+Release %.0f ns/call over %d calls\n",
                double(c1.QuadPart - c0.QuadPart) * 1e9 / double(f0.QuadPart) / kIters, kIters);
    input->Release();
    return 0;
}
}  // namespace

// A scripted press is held until the title has asked on four separate frames (at most 1.5 s), so a stalled frame
// cannot hide it or show it once.
int sticky_press(CreateFn create) {
    SetEnvironmentVariableA("LEONIDA_INPUT_SCRIPT", "a@1");
    rt::gameinput_mac::set_clock(fake_clock);
    at(0);
    IGameInput* input = nullptr;
    check(create(reinterpret_cast<void**>(&input)) == S_OK && input, "sticky: GameInputCreate returns an instance");
    if (!input) return 1;
    check(read_at(input, nullptr, 0.5).buttons == 0, "sticky: A is up before its time");
    check(read_at(input, nullptr, 1.0).buttons & GameInputGamepadA, "sticky: A is down at 1.0 s (frame 1)");
    check(read_at(input, nullptr, 1.4).buttons & GameInputGamepadA,
          "sticky: past the 0.3 s hold A stays down, the title has asked on one frame only");
    check(read_at(input, nullptr, 1.401).buttons & GameInputGamepadA, "sticky: a call 1 ms later is the same frame, still down");
    check(read_at(input, nullptr, 1.42).buttons & GameInputGamepadA, "sticky: frame 3, still down");
    check(read_at(input, nullptr, 1.44).buttons & GameInputGamepadA, "sticky: frame 4, still down on that ask");
    check(read_at(input, nullptr, 1.46).buttons == 0, "sticky: released after four frames saw it");
    check(read_at(input, nullptr, 1.9).buttons == 0, "sticky: a press the title already saw is not delivered again");
    check(read_at(input, nullptr, 3.0).buttons == 0, "sticky: and stays released");
    check(read_at(input, nullptr, 5.0).buttons == 0, "sticky: until the script says otherwise");
    input->Release();
    return 0;
}

// A press the title never asked for during its hold (a 3 s frame) is delivered late, once, and seen on four frames.
int late_press(CreateFn create) {
    SetEnvironmentVariableA("LEONIDA_INPUT_SCRIPT", "a@1,b@20");
    rt::gameinput_mac::set_clock(fake_clock);
    at(0);
    IGameInput* input = nullptr;
    check(create(reinterpret_cast<void**>(&input)) == S_OK && input, "late: GameInputCreate returns an instance");
    if (!input) return 1;
    check(read_at(input, nullptr, 0.9).buttons == 0, "late: nothing before the press");
    check(read_at(input, nullptr, 4.0).buttons & GameInputGamepadA, "late: the title's next frame, 3 s after the hold, still reads A");
    check(read_at(input, nullptr, 4.02).buttons & GameInputGamepadA, "late: and the next");
    check(read_at(input, nullptr, 4.04).buttons & GameInputGamepadA, "late: and the next");
    check(read_at(input, nullptr, 4.06).buttons & GameInputGamepadA, "late: fourth frame");
    check(read_at(input, nullptr, 4.08).buttons == 0, "late: then released");
    check(read_at(input, nullptr, 5.0).buttons == 0, "late: and not delivered again");
    check(read_at(input, nullptr, 31.0).buttons == 0, "late: a press more than 10 s old is dropped, not delivered");
    input->Release();
    return 0;
}

// Script and live devices together: with no device named the title sees whatever is held, whoever holds it.
int alongside(CreateFn create) {
    SetEnvironmentVariableA("LEONIDA_INPUT_SCRIPT", "a@1");
    SetEnvironmentVariableA("GTA6RT_SCRIPT_MIN_POLLS", "0");
    rt::gameinput_mac::set_clock(fake_clock);
    at(0);
    IGameInput* input = nullptr;
    check(create(reinterpret_cast<void**>(&input)) == S_OK && input, "alongside: GameInputCreate returns an instance");
    if (!input) return 1;
    rt::gameinput_mac::PadDesc desc;
    desc.key = "test:live";
    desc.name = "Live pad";
    const uint64_t live = rt::gameinput_mac::sink().connect(desc);
    check(live != 0, "alongside: a live pad connects next to the scripted one");
    rt::gameinput_mac::PadState held;
    held.ly = 1.0f;
    held.rx = 0.5f;
    at(0.5);
    rt::gameinput_mac::sink().state(live, held, false);
    check(read_at(input, nullptr, 0.6).right_y == 0, "alongside: the live pad's reading is served (the script is idle)");
    auto left_y = [&](double t) {
        at(t);
        IGameInputReading* r = nullptr;
        GameInputGamepadState s{};
        s.leftThumbstickY = -9;
        if (input->GetCurrentReading(GameInputKindGamepad, nullptr, &r) == S_OK && r) {
            r->GetGamepadState(&s);
            r->Release();
        }
        return s;
    };
    check(left_y(0.7).leftThumbstickY == 1.0f, "alongside: a held live stick is what the title reads");
    check(left_y(1.05).buttons & GameInputGamepadA, "alongside: the script's A wins while it is the only one pressing");
    check(left_y(1.5).leftThumbstickY == 1.0f,
          "alongside: after the script's release the held live stick is not hidden by the newer idle reading");
    rt::gameinput_mac::PadState idle;
    rt::gameinput_mac::sink().state(live, idle, false);
    check(left_y(2.0).leftThumbstickY == 0.0f, "alongside: all idle reads idle");
    input->Release();
    return 0;
}

int main(int argc, char** argv) {
    GDK_TEST_REGISTER();
    auto create = reinterpret_cast<CreateFn>(rt::shims::find("gameinput.dll", "#1"));
    auto named = reinterpret_cast<CreateFn>(rt::shims::find("gameinput.dll", "GameInputCreate"));
    check(create != nullptr, "the ordinal import gameinput.dll!#1 is served");
    check(named != nullptr, "the name GameInputCreate is served");
    if (!create) return 1;

    const bool with_script = argc == 2 && std::strcmp(argv[1], "--input-script") == 0;
    const bool with_synthetic = argc == 2 && std::strcmp(argv[1], "--synthetic") == 0;
    const bool with_census = argc == 2 && std::strcmp(argv[1], "--census") == 0;
    const bool with_sticky = argc == 2 && std::strcmp(argv[1], "--sticky") == 0;
    const bool with_late = argc == 2 && std::strcmp(argv[1], "--late") == 0;
    const bool with_alongside = argc == 2 && std::strcmp(argv[1], "--alongside") == 0;
    SetEnvironmentVariableA("LEONIDA_INPUT_SCRIPT", nullptr);
    SetEnvironmentVariableA("LEONIDA_INPUT_LOG", nullptr);
    SetEnvironmentVariableA("GTA6RT_SCRIPT_MIN_POLLS", with_script ? "0" : nullptr);
    SetEnvironmentVariableA("GTA6RT_INPUT", with_script || with_sticky || with_late ? "script" : with_alongside ? "test" : "none");
    const int early = with_script      ? scripted_pad(create)
                      : with_sticky    ? sticky_press(create)
                      : with_late      ? late_press(create)
                      : with_alongside ? alongside(create)
                      : with_synthetic ? synthetic(create)
                      : with_census    ? census(create)
                                       : identity_only(create);
    std::printf("%d failure(s)\n", g_failed);
    return early || g_failed ? 1 : 0;
}
