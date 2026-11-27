#pragma once

#include <gta6rt/abi.h>
#include <gta6rt/os.h>

#include <atomic>
#include <cstdint>

#define MLOG(level, mod, ...) gta6rt_log(GTA6RT_LOG_##level, mod, __VA_ARGS__)

// Logs the first call only; `flag` is a namespace-scope std::atomic<bool>.
#define MLOG_ONCE(flag, level, mod, ...)                                   \
    do {                                                                    \
        if (!(flag).exchange(true, std::memory_order_relaxed)) MLOG(level, mod, __VA_ARGS__); \
    } while (0)

namespace rt::media {

// Calls a guest function (a COM method of a guest object) with integer/pointer arguments. The default goes through
// gta6rt_isa_call_guest; tests install a direct invoker so host functions can stand in for the guest.
using GuestInvoker = uint64_t (*)(uint64_t fn, const uint64_t* args, uint32_t count);
void set_guest_invoker(GuestInvoker invoker);
uint64_t guest_call(uint64_t fn, const uint64_t* args, uint32_t count);

inline uint64_t guest_method(uint64_t object, uint32_t slot, const uint64_t* rest = nullptr, uint32_t rest_count = 0) {
    const uint64_t* vtbl = *reinterpret_cast<const uint64_t* const*>(object);
    uint64_t args[8] = {object};
    for (uint32_t i = 0; i < rest_count && i < 7; ++i) args[i + 1] = rest[i];
    return guest_call(vtbl[slot], args, rest_count + 1);
}

uint64_t direct_guest_invoker(uint64_t fn, const uint64_t* args, uint32_t count);

uint64_t now_ns();

}  // namespace rt::media
