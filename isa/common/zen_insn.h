#pragma once

// Zen 2 instructions Rosetta does not implement (SIGILL) and the title executes without a CPUID guard:
// CLWB (66 0F AE /6), CLFLUSHOPT (66 0F AE /7), CLZERO (0F 01 FC), MONITORX (0F 01 FA), MWAITX (0F 01 FB).
// On this host CLWB/CLFLUSHOPT and MONITORX/MWAITX have no observable effect; CLZERO zeroes the 64-byte line at rAX.

#include <cstddef>
#include <cstdint>

enum class ZenInsn : uint8_t { None, CacheWriteback, ClZero, MonitorX, MWaitX };

struct ZenDecoded {
    ZenInsn kind = ZenInsn::None;
    uint8_t length = 0;
    bool addr32 = false;
};

bool zen_decode(const uint8_t* p, size_t avail, ZenDecoded* out);
