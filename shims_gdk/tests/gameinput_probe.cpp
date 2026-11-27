// By hand, with a controller or the keyboard: prints devices and readings through the real GameController backend.
//   gameinput_probe [seconds]      (default 20; GTA6RT_INPUT_BACKGROUND=1 reads input while another app has focus)
// The main thread runs the run loop (GameController connect notifications arrive there) and polls readings.
#include <windows.h>

#include <cstdio>
#include <cstdlib>

#include "gameinput_abi.h"
#include "gameinput_backend.h"
#include <gta6rt/shims.h>

#include "shims.h"

using namespace gi;

namespace {
void CALLBACK on_device(GameInputCallbackToken, void*, IGameInputDevice* device, uint64_t, uint32_t current, uint32_t previous) {
    const GameInputDeviceInfo* info = device->GetDeviceInfo();
    std::printf("device \"%s\" status %#x -> %#x (vendor %#x product %#x, rumble motors %#x)\n",
                info->displayName ? info->displayName->data : "?", previous, current, info->vendorId, info->productId,
                info->supportedRumbleMotors);
    std::fflush(stdout);
}
}  // namespace

int main(int argc, char** argv) {
    GDK_TEST_REGISTER();
    const double seconds = argc > 1 ? std::atof(argv[1]) : 20.0;
    auto create = reinterpret_cast<HRESULT(WINAPI*)(void**)>(rt::shims::find("gameinput.dll", "#1"));
    IGameInput* input = nullptr;
    if (!create || create(reinterpret_cast<void**>(&input)) != S_OK || !input) return 1;
    GameInputCallbackToken token = 0;
    input->RegisterDeviceCallback(nullptr, GameInputKindGamepad, GameInputDeviceAnyStatus, GameInputAsyncEnumeration, nullptr,
                                  on_device, &token);
    GameInputGamepadState last{};
    uint64_t last_seq = 0;
    for (double t = 0; t < seconds; t += 0.01) {
        rt::gameinput_mac::pump_run_loop(0.01);
        IGameInputReading* r = nullptr;
        if (input->GetCurrentReading(GameInputKindGamepad, nullptr, &r) != S_OK || !r) continue;
        GameInputGamepadState s{};
        r->GetGamepadState(&s);
        const uint64_t seq = r->GetSequenceNumber(GameInputKindGamepad);
        if (seq != last_seq) {
            std::printf("t=%.2f seq %llu buttons %#06x lt %.2f rt %.2f L(%.2f,%.2f) R(%.2f,%.2f)\n", t,
                        static_cast<unsigned long long>(seq), s.buttons, s.leftTrigger, s.rightTrigger, s.leftThumbstickX,
                        s.leftThumbstickY, s.rightThumbstickX, s.rightThumbstickY);
            std::fflush(stdout);
            last_seq = seq;
            last = s;
        }
        r->Release();
    }
    (void)last;
    return 0;
}
