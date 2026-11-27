// GTA6RT_THUNK_NATIVE with a signature on arm64: the guest's Win64 call is re-marshalled into an AAPCS64 call (Apple
// variant: stack arguments packed at natural alignment, sub-32-bit integers extended by the caller, variadic arguments
// on the stack in 8-byte slots) of a plain host function, and the result is mapped back to rax/xmm0 or the Win64
// hidden return buffer.

#include "fexcore_internal.h"

#include <FEXCore/Core/X86Enums.h>

#include <cstring>

using X = FEXCore::X86State::X86Reg;

extern "C" void gta6rt_fex_native_call(void* regs);

namespace gta6rt::fex {

namespace {

struct alignas(16) NativeRegs {
    uint64_t x[8];
    uint8_t v[8][16];
    uint64_t stack;
    uint64_t stack_bytes;
    uint64_t x8;
    uint64_t fn;
};
static_assert(offsetof(NativeRegs, v) == 64 && offsetof(NativeRegs, stack) == 192 && offsetof(NativeRegs, x8) == 208 &&
              offsetof(NativeRegs, fn) == 216);

constexpr size_t kMaxStack = 512;

struct Builder {
    NativeRegs r{};
    unsigned ngrn = 0, nsrn = 0;
    size_t sp = 0;
    alignas(16) uint8_t stack[kMaxStack];
    bool overflow = false;

    void put_stack(const void* p, size_t n, size_t align) {
        sp = (sp + align - 1) & ~(align - 1);
        if (sp + n > kMaxStack) {
            overflow = true;
            return;
        }
        memcpy(stack + sp, p, n);
        sp += n;
    }
    void gpr(uint64_t v, size_t stack_size) {
        if (ngrn < 8) {
            r.x[ngrn++] = v;
            return;
        }
        put_stack(&v, stack_size, stack_size);
    }
    void fpr(const void* p, size_t n) {
        if (nsrn < 8) {
            memset(r.v[nsrn], 0, 16);
            memcpy(r.v[nsrn++], p, n);
            return;
        }
        put_stack(p, n, n);
    }
};

bool is_float(uint8_t t) {
    return t == GTA6RT_ABI_F32 || t == GTA6RT_ABI_F64;
}

uint64_t extend(uint8_t t, uint64_t v) {
    switch (t) {
    case GTA6RT_ABI_I32: return static_cast<uint32_t>(v);
    case GTA6RT_ABI_U32: return static_cast<uint32_t>(v);
    case GTA6RT_ABI_I16: return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v)));
    case GTA6RT_ABI_U16: return static_cast<uint16_t>(v);
    case GTA6RT_ABI_I8: return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(v)));
    case GTA6RT_ABI_U8: return static_cast<uint8_t>(v);
    default: return v;
    }
}

size_t int_stack_size(uint8_t t) {
    switch (t) {
    case GTA6RT_ABI_I32:
    case GTA6RT_ABI_U32: return 4;
    case GTA6RT_ABI_I16:
    case GTA6RT_ABI_U16: return 2;
    case GTA6RT_ABI_I8:
    case GTA6RT_ABI_U8: return 1;
    default: return 8;
    }
}

bool win_by_value(uint16_t size) {
    return size == 1 || size == 2 || size == 4 || size == 8;
}

unsigned hfa_count(const gta6rt_abi_arg& a) {
    return a.hfa & 0xF;
}

size_t hfa_member(const gta6rt_abi_arg& a) {
    return (a.hfa & GTA6RT_ABI_HFA_F64) ? 8 : 4;
}

}  // namespace

bool native_call(const NativeThunk* t, FEXCore::Core::CpuStateFrame* frame) {
    const gta6rt_native_sig& sig = t->sig;
    auto& s = frame->State;
    const uint64_t rsp = s.gregs[X::REG_RSP];
    static constexpr X kWinRegs[4] = {X::REG_RCX, X::REG_RDX, X::REG_R8, X::REG_R9};
    unsigned slot = 0;
    auto win_int = [&](unsigned k) -> uint64_t {
        if (k < 4) return s.gregs[kWinRegs[k]];
        return reinterpret_cast<const uint64_t*>(rsp + 0x28)[k - 4];
    };
    auto win_flt = [&](unsigned k) -> uint64_t {
        if (k < 4) return s.xmm.sse.data[k][0];
        return reinterpret_cast<const uint64_t*>(rsp + 0x28)[k - 4];
    };

    Builder b;
    const bool agg_ret = sig.ret.type == GTA6RT_ABI_AGG;
    const bool win_hidden = agg_ret && !win_by_value(sig.ret.size);
    uint64_t hidden = 0;
    if (win_hidden) hidden = win_int(slot++);
    const bool aapcs_x8 = agg_ret && !hfa_count(sig.ret) && sig.ret.size > 16;
    if (aapcs_x8) b.r.x8 = hidden;

    for (unsigned i = 0; i < sig.count; ++i, ++slot) {
        const gta6rt_abi_arg& a = sig.args[i];
        if (sig.fixed != GTA6RT_ABI_NOT_VARIADIC && i >= sig.fixed) {
            const uint64_t v = is_float(a.type) ? win_flt(slot) : win_int(slot);
            uint64_t w = v;
            if (a.type == GTA6RT_ABI_F32) {
                float f;
                uint32_t u = static_cast<uint32_t>(v);
                memcpy(&f, &u, 4);
                const double d = f;
                memcpy(&w, &d, 8);
            }
            b.put_stack(&w, 8, 8);
            continue;
        }
        switch (a.type) {
        case GTA6RT_ABI_F32: {
            const uint32_t u = static_cast<uint32_t>(win_flt(slot));
            b.fpr(&u, 4);
            break;
        }
        case GTA6RT_ABI_F64: {
            const uint64_t u = win_flt(slot);
            b.fpr(&u, 8);
            break;
        }
        case GTA6RT_ABI_M128: {
            const auto* p = reinterpret_cast<const uint8_t*>(win_int(slot));
            if (b.nsrn < 8) {
                memcpy(b.r.v[b.nsrn++], p, 16);
            } else {
                b.put_stack(p, 16, 16);
            }
            break;
        }
        case GTA6RT_ABI_AGG: {
            const uint64_t raw = win_int(slot);
            alignas(16) uint8_t buf[64] = {};
            const size_t n = a.size;
            if (win_by_value(a.size)) {
                memcpy(buf, &raw, n);
            } else if (n <= sizeof buf) {
                memcpy(buf, reinterpret_cast<const void*>(raw), n);
            }
            if (const unsigned k = hfa_count(a)) {
                const size_t m = hfa_member(a);
                if (b.nsrn + k <= 8) {
                    for (unsigned j = 0; j < k; ++j) b.fpr(buf + j * m, m);
                } else {
                    b.nsrn = 8;
                    b.put_stack(buf, k * m, m);
                }
            } else if (n > 16) {
                b.gpr(win_by_value(a.size) ? reinterpret_cast<uint64_t>(buf) : raw, 8);
            } else {
                const unsigned words = static_cast<unsigned>((n + 7) / 8);
                if (b.ngrn + words <= 8) {
                    for (unsigned j = 0; j < words; ++j) {
                        uint64_t w;
                        memcpy(&w, buf + 8 * j, 8);
                        b.r.x[b.ngrn++] = w;
                    }
                } else {
                    b.ngrn = 8;
                    b.put_stack(buf, words * 8, 8);
                }
            }
            break;
        }
        default:
            b.gpr(extend(a.type, win_int(slot)), int_stack_size(a.type));
            break;
        }
    }
    if (b.overflow) return false;
    b.r.stack = reinterpret_cast<uint64_t>(b.stack);
    b.r.stack_bytes = (b.sp + 15) & ~size_t(15);
    b.r.fn = reinterpret_cast<uint64_t>(t->fn);
    gta6rt_fex_native_call(&b.r);

    switch (sig.ret.type) {
    case GTA6RT_ABI_VOID: break;
    case GTA6RT_ABI_F32:
    case GTA6RT_ABI_F64: {
        uint64_t lo;
        memcpy(&lo, b.r.v[0], 8);
        if (sig.ret.type == GTA6RT_ABI_F32) lo &= 0xFFFFFFFFull;
        s.xmm.sse.data[0][0] = lo;
        s.xmm.sse.data[0][1] = 0;
        break;
    }
    case GTA6RT_ABI_M128:
        memcpy(&s.xmm.sse.data[0][0], b.r.v[0], 16);
        break;
    case GTA6RT_ABI_AGG: {
        alignas(16) uint8_t buf[64] = {};
        const size_t n = sig.ret.size;
        if (const unsigned k = hfa_count(sig.ret)) {
            const size_t m = hfa_member(sig.ret);
            for (unsigned j = 0; j < k; ++j) memcpy(buf + j * m, b.r.v[j], m);
        } else if (n <= 16) {
            memcpy(buf, &b.r.x[0], 8);
            memcpy(buf + 8, &b.r.x[1], 8);
        }
        if (win_hidden) {
            if (!aapcs_x8) memcpy(reinterpret_cast<void*>(hidden), buf, n);
            s.gregs[X::REG_RAX] = hidden;
        } else {
            uint64_t v = 0;
            memcpy(&v, buf, n);
            s.gregs[X::REG_RAX] = v;
        }
        break;
    }
    default:
        s.gregs[X::REG_RAX] = b.r.x[0];
        break;
    }
    return true;
}

}  // namespace gta6rt::fex
