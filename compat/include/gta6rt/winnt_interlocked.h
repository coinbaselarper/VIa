#pragma once

/* Interlocked* and barriers on __atomic builtins (sequentially consistent, as on Windows). Portable to arm64. */

#include <gta6rt/winnt_base.h>

#define GTA6RT_IL static inline __attribute__((always_inline))

GTA6RT_IL LONG _InterlockedIncrement(LONG volatile* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
GTA6RT_IL LONG _InterlockedDecrement(LONG volatile* p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
GTA6RT_IL LONG _InterlockedExchange(LONG volatile* p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
GTA6RT_IL LONG _InterlockedExchangeAdd(LONG volatile* p, LONG v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
GTA6RT_IL LONG _InterlockedOr(LONG volatile* p, LONG v) { return __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST); }
GTA6RT_IL LONG _InterlockedAnd(LONG volatile* p, LONG v) { return __atomic_fetch_and(p, v, __ATOMIC_SEQ_CST); }
GTA6RT_IL LONG _InterlockedXor(LONG volatile* p, LONG v) { return __atomic_fetch_xor(p, v, __ATOMIC_SEQ_CST); }
GTA6RT_IL LONG _InterlockedCompareExchange(LONG volatile* p, LONG x, LONG c) {
    __atomic_compare_exchange_n(p, &c, x, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return c;
}
GTA6RT_IL short _InterlockedIncrement16(short volatile* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
GTA6RT_IL short _InterlockedDecrement16(short volatile* p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
GTA6RT_IL short _InterlockedCompareExchange16(short volatile* p, short x, short c) {
    __atomic_compare_exchange_n(p, &c, x, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return c;
}
GTA6RT_IL char _InterlockedExchange8(char volatile* p, char v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
GTA6RT_IL char _InterlockedCompareExchange8(char volatile* p, char x, char c) {
    __atomic_compare_exchange_n(p, &c, x, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return c;
}
GTA6RT_IL LONG64 _InterlockedIncrement64(LONG64 volatile* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
GTA6RT_IL LONG64 _InterlockedDecrement64(LONG64 volatile* p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
GTA6RT_IL LONG64 _InterlockedExchange64(LONG64 volatile* p, LONG64 v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
GTA6RT_IL LONG64 _InterlockedExchangeAdd64(LONG64 volatile* p, LONG64 v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
GTA6RT_IL LONG64 _InterlockedOr64(LONG64 volatile* p, LONG64 v) { return __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST); }
GTA6RT_IL LONG64 _InterlockedAnd64(LONG64 volatile* p, LONG64 v) { return __atomic_fetch_and(p, v, __ATOMIC_SEQ_CST); }
GTA6RT_IL LONG64 _InterlockedCompareExchange64(LONG64 volatile* p, LONG64 x, LONG64 c) {
    __atomic_compare_exchange_n(p, &c, x, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return c;
}
GTA6RT_IL PVOID _InterlockedExchangePointer(PVOID volatile* p, PVOID v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
GTA6RT_IL PVOID _InterlockedCompareExchangePointer(PVOID volatile* p, PVOID x, PVOID c) {
    __atomic_compare_exchange_n(p, &c, x, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return c;
}
GTA6RT_IL BOOLEAN _InterlockedCompareExchange128(LONG64 volatile* p, LONG64 hi, LONG64 lo, LONG64* cmp) {
    __int128 expected = ((__int128)cmp[1] << 64) | (unsigned long long)cmp[0];
    __int128 desired = ((__int128)hi << 64) | (unsigned long long)lo;
    BOOLEAN ok = __atomic_compare_exchange_n((__int128 volatile*)p, &expected, desired, 0, __ATOMIC_SEQ_CST,
                                             __ATOMIC_SEQ_CST);
    cmp[0] = (LONG64)expected;
    cmp[1] = (LONG64)(expected >> 64);
    return ok;
}

#define InterlockedIncrement _InterlockedIncrement
#define InterlockedDecrement _InterlockedDecrement
#define InterlockedExchange _InterlockedExchange
#define InterlockedExchangeAdd _InterlockedExchangeAdd
#define InterlockedOr _InterlockedOr
#define InterlockedAnd _InterlockedAnd
#define InterlockedXor _InterlockedXor
#define InterlockedCompareExchange _InterlockedCompareExchange
#define InterlockedIncrement16 _InterlockedIncrement16
#define InterlockedDecrement16 _InterlockedDecrement16
#define InterlockedCompareExchange16 _InterlockedCompareExchange16
#define InterlockedIncrement64 _InterlockedIncrement64
#define InterlockedDecrement64 _InterlockedDecrement64
#define InterlockedExchange64 _InterlockedExchange64
#define InterlockedExchangeAdd64 _InterlockedExchangeAdd64
#define InterlockedOr64 _InterlockedOr64
#define InterlockedAnd64 _InterlockedAnd64
#define InterlockedCompareExchange64 _InterlockedCompareExchange64
#define InterlockedExchangePointer _InterlockedExchangePointer
#define InterlockedCompareExchangePointer _InterlockedCompareExchangePointer
#define InterlockedCompareExchange128 _InterlockedCompareExchange128
#define InterlockedIncrementAcquire InterlockedIncrement
#define InterlockedDecrementRelease InterlockedDecrement
#define InterlockedCompareExchangeAcquire InterlockedCompareExchange
#define InterlockedCompareExchangeRelease InterlockedCompareExchange
#define InterlockedExchangeNoFence InterlockedExchange

#define MemoryBarrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define _ReadWriteBarrier() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#define _ReadBarrier() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#define _WriteBarrier() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#if defined(GTA6RT_ARCH_X86_64)
#define YieldProcessor() __builtin_ia32_pause()
#else
#define YieldProcessor() __asm__ __volatile__("yield")
#endif
#define ReadAcquire(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define WriteRelease(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define ReadNoFence(p) __atomic_load_n((p), __ATOMIC_RELAXED)
#define WriteNoFence(p, v) __atomic_store_n((p), (v), __ATOMIC_RELAXED)

#undef GTA6RT_IL
