#include "zen_insn.h"

bool zen_decode(const uint8_t* p, size_t avail, ZenDecoded* out) {
    size_t i = 0;
    bool op66 = false, addr32 = false;
    while (i < avail && i < 4 && (p[i] == 0x66 || p[i] == 0x67)) {
        if (p[i] == 0x66) op66 = true;
        else addr32 = true;
        ++i;
    }
    if (i < avail && (p[i] & 0xF0) == 0x40) ++i;
    if (i + 3 > avail || p[i] != 0x0F) return false;
    if (p[i + 1] == 0x01) {
        const uint8_t m = p[i + 2];
        ZenInsn k = m == 0xFC ? ZenInsn::ClZero : m == 0xFA ? ZenInsn::MonitorX : m == 0xFB ? ZenInsn::MWaitX : ZenInsn::None;
        if (k == ZenInsn::None) return false;
        *out = ZenDecoded{k, static_cast<uint8_t>(i + 3), addr32};
        return true;
    }
    if (p[i + 1] != 0xAE || !op66) return false;
    const uint8_t modrm = p[i + 2];
    const unsigned mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
    if (mod == 3 || (reg != 6 && reg != 7)) return false;
    size_t len = i + 3;
    if (rm == 4) {
        if (len >= avail) return false;
        const uint8_t sib = p[len++];
        if (mod == 0 && (sib & 7) == 5) len += 4;
    }
    if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    else if (mod == 0 && rm == 5) len += 4;
    if (len > avail || len > 15) return false;
    *out = ZenDecoded{ZenInsn::CacheWriteback, static_cast<uint8_t>(len), addr32};
    return true;
}
