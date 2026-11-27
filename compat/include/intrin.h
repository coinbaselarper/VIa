#pragma once

/*
 * MSVC intrinsics. Portable ones work on both arches. x86-only host intrinsics (__cpuid, __rdtsc, _xgetbv,
 * __movsb) exist only on x86_64. GS-segment access (__readgsqword etc.) is deliberately absent: use
 * gta6rt_isa_current_teb() and friends from include/gta6rt/isa.h (X2: no Rosetta-specific code outside isa/).
 */

#include <stddef.h>
#include <string.h>
#include <gta6rt/winnt_interlocked.h>

#if defined(GTA6RT_ARCH_X86_64)
#include <immintrin.h>
/* clang's <cpuid.h> provides the MSVC-style __cpuidex function but a GCC-style 5-argument __cpuid macro;
 * the MSVC 2-argument __cpuid replaces that macro. Do not use GCC-style __cpuid after including intrin.h. */
#include <cpuid.h>
#undef __cpuid
static inline void __cpuid(int info[4], int leaf) { __cpuidex(info, leaf, 0); }
static inline unsigned long long gta6rt_xgetbv(unsigned int xcr) {
    unsigned int lo, hi;
    __asm__ __volatile__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(xcr));
    return ((unsigned long long)hi << 32) | lo;
}
#ifndef _xgetbv
#define _xgetbv gta6rt_xgetbv
#endif
static inline void __movsb(unsigned char* d, const unsigned char* s, size_t n) {
    __asm__ __volatile__("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
}
static inline void __stosb(unsigned char* d, unsigned char v, size_t n) {
    __asm__ __volatile__("rep stosb" : "+D"(d), "+c"(n) : "a"(v) : "memory");
}
#else
static inline void __movsb(unsigned char* d, const unsigned char* s, size_t n) { memmove(d, s, n); }
static inline void __stosb(unsigned char* d, unsigned char v, size_t n) { memset(d, v, n); }
#endif

#define _ReturnAddress() __builtin_return_address(0)
#define _AddressOfReturnAddress() ((void*)((char*)__builtin_frame_address(0) + sizeof(void*)))
#define __debugbreak() __builtin_debugtrap()
#define __fastfail(code) __builtin_trap()
#define _mm_pause_portable() YieldProcessor()

static inline unsigned char _BitScanForward(unsigned long* index, unsigned int mask) {
    if (!mask) return 0;
    *index = (unsigned long)__builtin_ctz(mask);
    return 1;
}
static inline unsigned char _BitScanReverse(unsigned long* index, unsigned int mask) {
    if (!mask) return 0;
    *index = 31ul - (unsigned long)__builtin_clz(mask);
    return 1;
}
static inline unsigned char _BitScanForward64(unsigned long* index, unsigned long long mask) {
    if (!mask) return 0;
    *index = (unsigned long)__builtin_ctzll(mask);
    return 1;
}
static inline unsigned char _BitScanReverse64(unsigned long* index, unsigned long long mask) {
    if (!mask) return 0;
    *index = 63ul - (unsigned long)__builtin_clzll(mask);
    return 1;
}
static inline unsigned int __popcnt(unsigned int v) { return (unsigned int)__builtin_popcount(v); }
static inline unsigned long long __popcnt64(unsigned long long v) { return (unsigned long long)__builtin_popcountll(v); }
static inline unsigned int __lzcnt(unsigned int v) { return v ? (unsigned int)__builtin_clz(v) : 32u; }
#ifndef __lzcnt64
static inline unsigned long long __lzcnt64(unsigned long long v) { return v ? (unsigned long long)__builtin_clzll(v) : 64ull; }
#endif
static inline unsigned long long __umulh(unsigned long long a, unsigned long long b) {
    return (unsigned long long)(((unsigned __int128)a * b) >> 64);
}
static inline unsigned long long _umul128(unsigned long long a, unsigned long long b, unsigned long long* hi) {
    unsigned __int128 r = (unsigned __int128)a * b;
    *hi = (unsigned long long)(r >> 64);
    return (unsigned long long)r;
}
