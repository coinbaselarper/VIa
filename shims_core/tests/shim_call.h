#pragma once

// Calls a registered shim the way the guest would: through the ms_abi function pointer on x86_64, through a packed
// gta6rt_guest_frame and the frame adapter on arm64.
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "shims.h"

namespace tcall {

#if defined(__x86_64__)

template <class R, class... A>
R call(const char* dll, const char* name, A... a) {
    const rt::shims::Found f = rt::shims::find_entry(dll, name);
    using Fn = R(__attribute__((ms_abi)) *)(A...);
    return reinterpret_cast<Fn>(f.fn)(a...);
}

#else

template <class T>
void pack_one(gta6rt_guest_frame& f, uint64_t* stack, unsigned index, T v) {
    uint64_t raw = 0;
    const bool is_float = std::is_floating_point_v<T>;
    if constexpr (std::is_floating_point_v<T>) {
        std::memcpy(&raw, &v, sizeof(T));
    } else if constexpr (std::is_pointer_v<T>) {
        raw = reinterpret_cast<uintptr_t>(v);
    } else if constexpr (std::is_signed_v<T>) {
        raw = static_cast<uint64_t>(static_cast<int64_t>(v));
    } else {
        raw = static_cast<uint64_t>(v);
    }
    if (index >= 4) {
        stack[5 + (index - 4)] = raw;
        return;
    }
    uint64_t* ints[4] = {&f.rcx, &f.rdx, &f.r8, &f.r9};
    uint64_t* floats[4] = {&f.xmm0, &f.xmm1, &f.xmm2, &f.xmm3};
    *(is_float ? floats[index] : ints[index]) = raw;
}

template <class R, class... A>
R call(const char* dll, const char* name, A... a) {
    const rt::shims::Found f = rt::shims::find_entry(dll, name);
    uint64_t stack[64] = {};
    gta6rt_guest_frame fr{};
    fr.rsp = reinterpret_cast<uintptr_t>(stack);
    unsigned i = 0;
    (pack_one(fr, stack, i++, a), ...);
    f.frame(&fr, nullptr);
    if constexpr (std::is_void_v<R>) {
        return;
    } else if constexpr (std::is_same_v<R, float>) {
        float v;
        const uint32_t bits = static_cast<uint32_t>(fr.xmm0_ret);
        std::memcpy(&v, &bits, 4);
        return v;
    } else if constexpr (std::is_same_v<R, double>) {
        double v;
        std::memcpy(&v, &fr.xmm0_ret, 8);
        return v;
    } else if constexpr (std::is_pointer_v<R>) {
        return reinterpret_cast<R>(static_cast<uintptr_t>(fr.rax));
    } else {
        return static_cast<R>(fr.rax);
    }
}

#endif


}  // namespace tcall

#include "env_stub.h"

namespace tcall {

// Calls a guest thunk address (a vtable slot the host handed out) the way the guest would.
template <class R, class... A>
R call_thunk(gta6rt_gva addr, A... a) {
    const stub::ThunkRecord* t = stub::thunk(addr);
#if defined(__x86_64__)
    using Fn = R(__attribute__((ms_abi)) *)(A...);
    return reinterpret_cast<Fn>(const_cast<void*>(t->fn))(a...);
#else
    uint64_t stack[64] = {};
    gta6rt_guest_frame fr{};
    fr.rsp = reinterpret_cast<uintptr_t>(stack);
    unsigned i = 0;
    (pack_one(fr, stack, i++, a), ...);
    reinterpret_cast<gta6rt_frame_handler>(const_cast<void*>(t->fn))(&fr, t->user);
    if constexpr (std::is_void_v<R>) return;
    else if constexpr (std::is_pointer_v<R>) return reinterpret_cast<R>(static_cast<uintptr_t>(fr.rax));
    else return static_cast<R>(fr.rax);
#endif
}

}  // namespace tcall
