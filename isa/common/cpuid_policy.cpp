#include "isa_internal.h"

namespace gta6rt::isa {

static void put(uint32_t& reg, unsigned bit, bool on) {
    if (on)
        reg |= 1u << bit;
    else
        reg &= ~(1u << bit);
}

void cpuid_apply_policy(uint32_t leaf, uint32_t subleaf, uint64_t f, gta6rt_cpuid_regs* r) {
    const bool avx = f & GTA6RT_CPUID_AVX;
    switch (leaf) {
    case 0:
        if (r->eax < 0xD) r->eax = 0xD;
        break;
    case 1:
        put(r->ecx, 12, avx && (f & GTA6RT_CPUID_FMA3));
        put(r->ecx, 22, f & GTA6RT_CPUID_MOVBE);
        put(r->ecx, 26, avx);
        put(r->ecx, 27, avx);
        put(r->ecx, 28, avx);
        put(r->ecx, 29, avx && (f & GTA6RT_CPUID_F16C));
        break;
    case 7:
        if (subleaf == 0) {
            put(r->ebx, 0, false);  // FSGSBASE: rd/wrgsbase SIGILL under Rosetta
            put(r->ebx, 3, f & GTA6RT_CPUID_BMI1);
            put(r->ebx, 5, avx && (f & GTA6RT_CPUID_AVX2));
            put(r->ebx, 8, f & GTA6RT_CPUID_BMI2);
            put(r->ebx, 18, false);  // RDSEED
            put(r->ebx, 19, f & GTA6RT_CPUID_ADX);
            put(r->ebx, 24, false);  // CLWB
            put(r->ebx, 29, f & GTA6RT_CPUID_SHA);
            const bool a512 = f & GTA6RT_CPUID_AVX512;
            for (unsigned b : {16u, 17u, 21u, 26u, 27u, 28u, 30u, 31u}) put(r->ebx, b, a512);
            for (unsigned b : {1u, 6u, 11u, 12u, 14u}) put(r->ecx, b, a512);
            put(r->ecx, 9, f & GTA6RT_CPUID_VAES);
            put(r->ecx, 10, false);  // VPCLMULQDQ
            put(r->ecx, 22, false);  // RDPID
            for (unsigned b : {2u, 3u, 8u, 23u}) put(r->edx, b, a512);
        } else if (subleaf == 1) {
            put(r->eax, 4, false);  // AVX-VNNI
            put(r->eax, 5, false);  // AVX512_BF16
        }
        break;
    case 0xD:
        if (subleaf == 0) {
            const uint32_t keep = avx ? 0x7u : 0x3u;
            r->eax = (r->eax & keep) | keep;
            r->edx = 0;
            const uint32_t size = avx ? 0x340u : 0x240u;
            r->ebx = size;
            if (r->ecx < size || r->ecx > 0x340u) r->ecx = size;
        } else if (subleaf == 1) {
            put(r->eax, 1, false);  // XSAVEC
            put(r->eax, 3, false);  // XSAVES
        } else if (subleaf == 2) {
            if (avx) {
                r->eax = 256;
                r->ebx = 576;
                r->ecx = 0;
                r->edx = 0;
            } else {
                *r = {0, 0, 0, 0};
            }
        } else if (subleaf >= 3) {
            *r = {0, 0, 0, 0};
        }
        break;
    case 0x80000000:
        if (r->eax < 0x80000008) r->eax = 0x80000008;
        break;
    case 0x80000001:
        put(r->ecx, 5, f & GTA6RT_CPUID_LZCNT);
        put(r->ecx, 6, f & GTA6RT_CPUID_SSE4A);
        put(r->ecx, 11, false);  // XOP
        put(r->ecx, 16, false);  // FMA4
        put(r->ecx, 21, false);  // TBM
        put(r->ecx, 29, false);  // MONITORX
        break;
    case 0x80000008:
        put(r->ebx, 0, false);  // CLZERO
        break;
    default:
        break;
    }
}

uint64_t policy_xcr0(uint64_t features, uint64_t host_xcr0) {
    (void)host_xcr0;
    return (features & GTA6RT_CPUID_AVX) ? 0x7 : 0x3;
}

}  // namespace gta6rt::isa
