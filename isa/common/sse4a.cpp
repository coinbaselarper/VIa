#include "sse4a.h"

namespace gta6rt::isa {

bool sse4a_decode(const uint8_t* p, size_t avail, Sse4aInsn* out) {
    size_t i = 0;
    uint8_t pfx = 0;
    while (i < avail && (p[i] == 0x66 || p[i] == 0xF2)) {
        if (pfx && pfx != p[i]) return false;
        pfx = p[i++];
    }
    if (!pfx) return false;
    uint8_t rex = 0;
    if (i < avail && (p[i] & 0xF0) == 0x40) rex = p[i++];
    if (i + 3 > avail || p[i] != 0x0F || (p[i + 1] != 0x78 && p[i + 1] != 0x79)) return false;
    const bool imm = p[i + 1] == 0x78;
    const uint8_t modrm = p[i + 2];
    i += 3;
    if ((modrm >> 6) != 3) return false;
    const uint8_t reg = ((modrm >> 3) & 7) | ((rex & 4) ? 8 : 0);
    const uint8_t rm = (modrm & 7) | ((rex & 1) ? 8 : 0);
    Sse4aInsn r;
    if (pfx == 0x66) {
        if (imm) {
            if (((modrm >> 3) & 7) != 0) return false;
            r.op = Sse4aOp::kExtrqImm;
            r.dst = rm;
        } else {
            r.op = Sse4aOp::kExtrqReg;
            r.dst = reg;
            r.src = rm;
        }
    } else {
        r.op = imm ? Sse4aOp::kInsertqImm : Sse4aOp::kInsertqReg;
        r.dst = reg;
        r.src = rm;
    }
    if (imm) {
        if (i + 2 > avail) return false;
        r.len = p[i] & 63;
        r.idx = p[i + 1] & 63;
        i += 2;
    }
    r.length = static_cast<uint8_t>(i);
    *out = r;
    return true;
}

static uint64_t field_mask(unsigned len) {
    len &= 63;
    return len == 0 ? ~0ull : ((1ull << len) - 1);
}

uint64_t sse4a_extrq(uint64_t v, unsigned len, unsigned idx) {
    return (v >> (idx & 63)) & field_mask(len);
}

uint64_t sse4a_insertq(uint64_t dst, uint64_t src, unsigned len, unsigned idx) {
    const uint64_t m = field_mask(len);
    const unsigned s = idx & 63;
    return (dst & ~(m << s)) | ((src & m) << s);
}

void sse4a_execute(const Sse4aInsn& in, uint64_t (*xmm)[2]) {
    uint64_t* d = xmm[in.dst];
    const uint64_t* s = xmm[in.src];
    switch (in.op) {
    case Sse4aOp::kExtrqImm:
        d[0] = sse4a_extrq(d[0], in.len, in.idx);
        break;
    case Sse4aOp::kExtrqReg:
        d[0] = sse4a_extrq(d[0], s[0] & 63, (s[0] >> 8) & 63);
        break;
    case Sse4aOp::kInsertqImm:
        d[0] = sse4a_insertq(d[0], s[0], in.len, in.idx);
        break;
    case Sse4aOp::kInsertqReg:
        d[0] = sse4a_insertq(d[0], s[0], s[1] & 63, (s[1] >> 8) & 63);
        break;
    case Sse4aOp::kNone:
        break;
    }
}

}  // namespace gta6rt::isa
