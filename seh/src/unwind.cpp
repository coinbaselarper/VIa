#include <string.h>

#include "seh_internal.h"

namespace seh {

namespace {

enum {
    UWOP_PUSH_NONVOL = 0,
    UWOP_ALLOC_LARGE,
    UWOP_ALLOC_SMALL,
    UWOP_SET_FPREG,
    UWOP_SAVE_NONVOL,
    UWOP_SAVE_NONVOL_FAR,
    UWOP_EPILOG,
    UWOP_SPARE,
    UWOP_SAVE_XMM128,
    UWOP_SAVE_XMM128_FAR,
    UWOP_PUSH_MACHFRAME
};

struct mem {
    bool safe;
    bool ok;
    void rd(uint64_t a, void* out, size_t n) {
        if (safe) {
            if (!safe_read(a, out, n)) {
                memset(out, 0, n);
                ok = false;
            }
        } else {
            memcpy(out, reinterpret_cast<const void*>(a), n);
        }
    }
    uint64_t r64(uint64_t a) {
        uint64_t v;
        rd(a, &v, 8);
        return v;
    }
};

struct uinfo {
    uint8_t version, flags, prolog, count, frame_reg, frame_off;
    uint16_t codes[256 + 8];
};

int op_slots(uint16_t c, int version) {
    switch ((c >> 8) & 15) {
    case UWOP_ALLOC_LARGE: return (c >> 12) ? 3 : 2;
    case UWOP_SAVE_NONVOL:
    case UWOP_SAVE_XMM128: return 2;
    case UWOP_SAVE_NONVOL_FAR:
    case UWOP_SAVE_XMM128_FAR: return 3;
    case UWOP_EPILOG: return version >= 2 ? 1 : 2;
    case UWOP_SPARE: return 3;
    default: return 1;
    }
}

bool read_uinfo(uint64_t addr, mem& m, uinfo* u) {
    uint8_t h[4];
    m.rd(addr, h, 4);
    if (!m.ok) return false;
    u->version = h[0] & 7;
    u->flags = h[0] >> 3;
    u->prolog = h[1];
    u->count = h[2];
    u->frame_reg = h[3] & 15;
    u->frame_off = h[3] >> 4;
    const size_t slots = ((u->count + 1u) & ~1u) + 6;
    m.rd(addr + 4, u->codes, slots * 2);
    return m.ok && (u->version == 1 || u->version == 2);
}

void resolve_indirect(rf_t& f, uint64_t base, mem& m) {
    for (int k = 0; (f.unwind_info_rva & 1) && k < 4 && m.ok; ++k) m.rd(base + (f.unwind_info_rva & ~1u), &f, sizeof f);
}

const rf_t& chained_parent(const uinfo& u) { return *reinterpret_cast<const rf_t*>(&u.codes[(u.count + 1u) & ~1u]); }

// Begin RVA of the unchained (primary) entry of the function f belongs to.
bool root_begin(uint64_t base, rf_t f, mem& m, uint32_t* out) {
    for (int depth = 0; depth < 32; ++depth) {
        resolve_indirect(f, base, m);
        uinfo u;
        if (!read_uinfo(base + f.unwind_info_rva, m, &u)) return false;
        if (!(u.flags & kFlagChainInfo)) {
            *out = f.begin_rva;
            return true;
        }
        memcpy(&f, &chained_parent(u), sizeof f);
    }
    return false;
}

uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

bool is_rex(uint8_t b) { return (b & 0xf0) == 0x40; }

size_t stack_restore_length(const uint8_t* p) {
    if ((p[0] & 0xf8) != 0x48) return 0;
    if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xc4) return 4;
    if (p[0] == 0x48 && p[1] == 0x81 && p[2] == 0xc4) return 7;
    if (p[1] == 0x8d && !(p[0] & 0x06) && ((p[2] >> 3) & 7) == 4 && (p[2] & 7) != 4) {
        if ((p[2] >> 6) == 1) return 4;
        if ((p[2] >> 6) == 2) return 7;
    }
    return 0;
}

// Windows x64 epilog rule (RtlVirtualUnwind): optional add/lea rsp, any pops, then ret, rep ret, a jump
// out of the function, jmp [rip+x], or rex.w jmp r/m.
bool epilog_shape(const uint8_t* p, uint64_t pc, uint64_t base, const rf_t& f, bool chained, mem& m) {
    size_t i = 0;
    if ((p[0] & 0xf8) == 0x48 && (p[1] == 0x81 || p[1] == 0x83 || p[1] == 0x8d)) {
        i = stack_restore_length(p);
        if (!i) return false;
    }
    while (i < 30) {
        if ((p[i] & 0xf8) == 0x58) {
            i += 1;
        } else if (is_rex(p[i]) && (p[i + 1] & 0xf8) == 0x58) {
            i += 2;
        } else {
            break;
        }
    }
    if (i >= 30) return false;
    const uint8_t b = p[i];
    if (b == 0xc3 || b == 0xc2) return true;
    if (b == 0xf3 && p[i + 1] == 0xc3) return true;
    if (b == 0xe9 || b == 0xeb) {
        const int64_t d = b == 0xe9 ? static_cast<int64_t>(static_cast<int32_t>(rd32(p + i + 1)))
                                    : static_cast<int64_t>(static_cast<int8_t>(p[i + 1]));
        const uint64_t target = pc + i + (b == 0xe9 ? 5 : 2) + d;
        const uint64_t t = target - base;
        if (t < f.begin_rva || t >= f.end_rva) {
            uint64_t tb = 0;
            const rf_t* tf = lookup(target, &tb);
            uint32_t r_self = 0, r_target = 0;
            if (!tf || tb != base) return true;
            mem mm{m.safe, true};
            rf_t tcopy;
            mm.rd(reinterpret_cast<uint64_t>(tf), &tcopy, sizeof tcopy);
            if (!mm.ok || !root_begin(base, f, mm, &r_self) || !root_begin(base, tcopy, mm, &r_target)) return true;
            if (r_self != r_target) return true;
            return t == r_target;
        }
        return t == f.begin_rva && !chained;
    }
    if (b == 0xff && p[i + 1] == 0x25) return true;
    if ((b & 0xf8) == 0x48 && p[i + 1] == 0xff && (p[i + 2] & 0x38) == 0x20) return true;
    return false;
}

void interpret_epilog(const uint8_t* p, ctx_t* c, nonvol_ptrs* ptrs, mem& m) {
    size_t i = stack_restore_length(p);
    if (i) {
        if (p[1] == 0x83) {
            c->rsp += static_cast<int64_t>(static_cast<int8_t>(p[3]));
        } else if (p[1] == 0x81) {
            c->rsp += static_cast<int64_t>(static_cast<int32_t>(rd32(p + 3)));
        } else {
            const unsigned reg = (p[2] & 7) | ((p[0] & 1) << 3);
            const int64_t disp = i == 4 ? static_cast<int64_t>(static_cast<int8_t>(p[3]))
                                        : static_cast<int64_t>(static_cast<int32_t>(rd32(p + 3)));
            c->rsp = *gpr(c, reg) + disp;
        }
    }
    while (i < 30) {
        unsigned reg;
        if ((p[i] & 0xf8) == 0x58) {
            reg = p[i] & 7u;
            i += 1;
        } else if (is_rex(p[i]) && (p[i + 1] & 0xf8) == 0x58) {
            reg = (p[i + 1] & 7u) | ((p[i] & 1u) << 3);
            i += 2;
        } else {
            break;
        }
        *gpr(c, reg) = m.r64(c->rsp);
        if (ptrs) ptrs->gp[reg] = reinterpret_cast<uint64_t*>(c->rsp);
        c->rsp += 8;
    }
    c->rip = m.r64(c->rsp);
    c->rsp += 8;
}

// Version 2 epilog descriptors (as emitted in the packaged vcruntime140/XCurl/libHttpClient): codes[0] holds the
// epilog size and, in OpInfo bit 0, whether an epilog ends at the function end; each following UWOP_EPILOG holds
// the distance from the function end to an epilog start (0 = padding). MSVC starts the described range after the
// stack deallocation, so only pops and the return are inside it.
bool in_v2_epilog(const uinfo& u, uint64_t pc, uint64_t base, const rf_t& f) {
    if (u.version < 2 || !u.count) return false;
    const uint16_t c0 = u.codes[0];
    if (((c0 >> 8) & 15) != UWOP_EPILOG) return false;
    const uint32_t size = c0 & 0xff;
    if (!size) return false;
    const uint64_t end = base + f.end_rva;
    if (((c0 >> 12) & 1) && pc >= end - size && pc < end) return true;
    for (unsigned i = 1; i < u.count && ((u.codes[i] >> 8) & 15) == UWOP_EPILOG; ++i) {
        const uint32_t off = (u.codes[i] & 0xffu) | (static_cast<uint32_t>(u.codes[i] >> 12) << 8);
        if (!off) continue;
        const uint64_t start = end - off;
        if (pc >= start && pc < start + size) return true;
    }
    return false;
}

void load_code(uint64_t pc, uint64_t fn_end, mem& m, uint8_t* p) {
    memset(p, 0xcc, 40);
    if (pc >= fn_end) return;
    size_t n = fn_end - pc;
    if (n > 32) n = 32;
    mem mm{m.safe, true};
    mm.rd(pc, p, n);
    if (!mm.ok) memset(p, 0xcc, 40);
}

}  // namespace

bool virtual_unwind(uint32_t type, uint64_t base, uint64_t pc, const rf_t* fn, ctx_t* c, nonvol_ptrs* ptrs, bool safe,
                    unwind_out* out) {
    mem m{safe, true};
    out->handler = 0;
    out->handler_data = nullptr;
    out->in_prolog = out->in_epilog = false;
    out->establisher = c->rsp;
    rf_t f;
    m.rd(reinterpret_cast<uint64_t>(fn), &f, sizeof f);
    resolve_indirect(f, base, m);
    if (!m.ok) return false;
    uinfo u;
    uint64_t info_addr = base + f.unwind_info_rva;
    if (!read_uinfo(info_addr, m, &u)) return false;

    const uint64_t begin = base + f.begin_rva;
    const bool in_prolog = pc >= begin && pc - begin < u.prolog;
    uint32_t prolog_offset = in_prolog ? static_cast<uint32_t>(pc - begin) : ~0u;

    uint64_t frame = c->rsp;
    if (u.frame_reg) {
        bool use_fp = !in_prolog || (u.flags & kFlagChainInfo);
        if (!use_fp)
            for (unsigned i = 0; i < u.count; i += op_slots(u.codes[i], u.version))
                if (((u.codes[i] >> 8) & 15) == UWOP_SET_FPREG) {
                    use_fp = prolog_offset >= (u.codes[i] & 0xffu);
                    break;
                }
        if (use_fp) frame = *gpr(c, u.frame_reg) - u.frame_off * 16ull;
    }
    out->establisher = frame;
    out->in_prolog = in_prolog;

    if (!in_prolog) {
        uint8_t code[40];
        load_code(pc, base + f.end_rva, m, code);
        if (in_v2_epilog(u, pc, base, f) || epilog_shape(code, pc, base, f, (u.flags & kFlagChainInfo) != 0, m)) {
            interpret_epilog(code, c, ptrs, m);
            out->in_epilog = true;
            return m.ok;
        }
    }

    bool mach = false;
    for (int depth = 0;; ++depth) {
        for (unsigned i = 0; i < u.count; i += op_slots(u.codes[i], u.version)) {
            const uint16_t code = u.codes[i];
            const unsigned op = (code >> 8) & 15, info = code >> 12;
            if (prolog_offset < (code & 0xffu)) continue;
            switch (op) {
            case UWOP_PUSH_NONVOL:
                *gpr(c, info) = m.r64(c->rsp);
                if (ptrs) ptrs->gp[info] = reinterpret_cast<uint64_t*>(c->rsp);
                c->rsp += 8;
                break;
            case UWOP_ALLOC_LARGE:
                c->rsp += info ? (u.codes[i + 1] | static_cast<uint32_t>(u.codes[i + 2]) << 16) : u.codes[i + 1] * 8u;
                break;
            case UWOP_ALLOC_SMALL: c->rsp += info * 8u + 8u; break;
            case UWOP_SET_FPREG: c->rsp = *gpr(c, u.frame_reg) - u.frame_off * 16ull; break;
            case UWOP_SAVE_NONVOL:
            case UWOP_SAVE_NONVOL_FAR: {
                const uint64_t a =
                    frame + (op == UWOP_SAVE_NONVOL ? u.codes[i + 1] * 8u
                                                    : (u.codes[i + 1] | static_cast<uint32_t>(u.codes[i + 2]) << 16));
                *gpr(c, info) = m.r64(a);
                if (ptrs) ptrs->gp[info] = reinterpret_cast<uint64_t*>(a);
                break;
            }
            case UWOP_SAVE_XMM128:
            case UWOP_SAVE_XMM128_FAR: {
                const uint64_t a =
                    frame + (op == UWOP_SAVE_XMM128 ? u.codes[i + 1] * 16u
                                                    : (u.codes[i + 1] | static_cast<uint32_t>(u.codes[i + 2]) << 16));
                m.rd(a, &c->fp.v.xmm[info], 16);
                if (ptrs) ptrs->fp[info] = reinterpret_cast<void*>(a);
                break;
            }
            case UWOP_PUSH_MACHFRAME:
                if (info) c->rsp += 8;
                c->rip = m.r64(c->rsp);
                c->rsp = m.r64(c->rsp + 24);
                mach = true;
                break;
            default: break;
            }
        }
        if (!(u.flags & kFlagChainInfo)) break;
        if (depth >= 32) return false;
        memcpy(&f, &chained_parent(u), sizeof f);
        resolve_indirect(f, base, m);
        info_addr = base + f.unwind_info_rva;
        if (!read_uinfo(info_addr, m, &u)) return false;
        prolog_offset = ~0u;
    }
    if (!mach) {
        c->rip = m.r64(c->rsp);
        c->rsp += 8;
    }
    if (!m.ok) return false;
    if (!(u.flags & type & (kFlagEHandler | kFlagUHandler))) return true;
    if (prolog_offset != ~0u) return true;
    uint32_t handler_rva;
    memcpy(&handler_rva, &chained_parent(u), 4);
    out->handler = base + handler_rva;
    out->handler_data = reinterpret_cast<void*>(info_addr + 4 + 2 * ((u.count + 1u) & ~1u) + 4);
    return true;
}

bool step_frame(ctx_t* c, bool safe, uint64_t* out_establisher) {
    if (!c->rip) return false;
    uint64_t base = 0;
    const rf_t* fn = lookup(c->rip, &base);
    if (fn) {
        unwind_out o;
        if (!virtual_unwind(0, base, c->rip, fn, c, nullptr, safe, &o)) return false;
        if (out_establisher) *out_establisher = o.establisher;
        return true;
    }
    if (!pc_in_guest_image(c->rip)) return false;
    if (out_establisher) *out_establisher = c->rsp;
    uint64_t ra;
    if (safe) {
        if (!safe_read(c->rsp, &ra, 8)) return false;
    } else {
        memcpy(&ra, reinterpret_cast<const void*>(c->rsp), 8);
    }
    c->rip = ra;
    c->rsp += 8;
    return true;
}

uint32_t backtrace(const ctx_t* ctx, uint64_t* pcs, uint32_t max) {
    if (!ctx || !pcs || !max) return 0;
    ctx_t c;
    memcpy(&c, ctx, sizeof c);
    uint32_t n = 0;
    while (n < max && c.rip) {
        pcs[n++] = c.rip;
        const uint64_t sp = c.rsp;
        if (!step_frame(&c, true, nullptr)) break;
        if (c.rsp <= sp) break;
    }
    return n;
}

}  // namespace seh
