#pragma once

#include <cstdint>
#include <cstring>
#include <tuple>
#include <type_traits>
#include <utility>

#include <gta6rt/isa.h>

namespace rt::shims::detail {

constexpr uint64_t kStackArgsOffset = 0x28;

inline uint64_t frame_stack_slot(const gta6rt_guest_frame* f, unsigned index) {
    uint64_t v;
    std::memcpy(&v, reinterpret_cast<const void*>(static_cast<uintptr_t>(f->rsp + kStackArgsOffset + 8ull * (index - 4))), 8);
    return v;
}

inline uint64_t frame_int_slot(const gta6rt_guest_frame* f, unsigned index) {
    switch (index) {
    case 0: return f->rcx;
    case 1: return f->rdx;
    case 2: return f->r8;
    case 3: return f->r9;
    default: return frame_stack_slot(f, index);
    }
}

inline uint64_t frame_float_slot(const gta6rt_guest_frame* f, unsigned index) {
    switch (index) {
    case 0: return f->xmm0;
    case 1: return f->xmm1;
    case 2: return f->xmm2;
    case 3: return f->xmm3;
    default: return frame_stack_slot(f, index);
    }
}

template <class T>
T frame_arg(const gta6rt_guest_frame* f, unsigned index) {
    if constexpr (std::is_same_v<T, float>) {
        const uint32_t bits = static_cast<uint32_t>(frame_float_slot(f, index));
        float v;
        std::memcpy(&v, &bits, 4);
        return v;
    } else if constexpr (std::is_same_v<T, double>) {
        const uint64_t bits = frame_float_slot(f, index);
        double v;
        std::memcpy(&v, &bits, 8);
        return v;
    } else if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<T>(static_cast<uintptr_t>(frame_int_slot(f, index)));
    } else if constexpr (std::is_same_v<T, bool>) {
        return (frame_int_slot(f, index) & 0xff) != 0;
    } else if constexpr (std::is_class_v<T> || std::is_union_v<T>) {
        static_assert(std::is_trivially_copyable_v<T> && (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8),
                      "Win64 passes only 1/2/4/8-byte aggregates in a register; larger ones arrive by pointer");
        const uint64_t raw = frame_int_slot(f, index);
        T v;
        std::memcpy(&v, &raw, sizeof(T));
        return v;
    } else {
        static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
        static_assert(sizeof(T) <= 8);
        return static_cast<T>(frame_int_slot(f, index));
    }
}

template <class R>
void frame_return(gta6rt_guest_frame* f, R v) {
    if constexpr (std::is_same_v<R, void>) {
    } else if constexpr (std::is_same_v<R, float>) {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        f->xmm0_ret = bits;
        f->ret_is_float = 1;
    } else if constexpr (std::is_same_v<R, double>) {
        std::memcpy(&f->xmm0_ret, &v, 8);
        f->ret_is_float = 1;
    } else if constexpr (std::is_pointer_v<R>) {
        f->rax = reinterpret_cast<uintptr_t>(v);
        f->ret_is_float = 0;
    } else if constexpr (std::is_signed_v<R>) {
        f->rax = static_cast<uint64_t>(static_cast<int64_t>(v));
        f->ret_is_float = 0;
    } else {
        f->rax = static_cast<uint64_t>(v);
        f->ret_is_float = 0;
    }
}

template <class Fn>
struct FrameTraits;

template <class R, class... A>
struct FrameTraits<R (*)(A...)> {
    using Ret = R;
    using Args = std::tuple<A...>;
    static constexpr size_t kCount = sizeof...(A);
};

#if defined(__aarch64__)
template <class R, class... A>
struct FrameTraits<R (*)(A..., ...)> {
    static_assert(sizeof...(A) < 0, "variadic guest calls have no frame adapter");
};
#endif

template <auto F, size_t... I>
void frame_invoke(gta6rt_guest_frame* f, std::index_sequence<I...>) {
    using Tr = FrameTraits<decltype(F)>;
    if constexpr (std::is_same_v<typename Tr::Ret, void>) {
        F(frame_arg<std::tuple_element_t<I, typename Tr::Args>>(f, static_cast<unsigned>(I))...);
    } else {
        frame_return(f, F(frame_arg<std::tuple_element_t<I, typename Tr::Args>>(f, static_cast<unsigned>(I))...));
    }
}

template <auto F>
void frame_handler(gta6rt_guest_frame* f, void*) {
    frame_invoke<F>(f, std::make_index_sequence<FrameTraits<decltype(F)>::kCount>{});
}

}  // namespace rt::shims::detail
