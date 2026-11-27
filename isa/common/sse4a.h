#pragma once

// AMD SSE4a insertq/extrq: decoder and bit-field semantics (AMD APM vol. 4). Rosetta raises SIGILL on these
// (spike/s0.2-avx); the title has 19 insertq sites (NOTES S0.6). The upper 64 bits of the destination are
// "undefined" per AMD; like QEMU (the S0.2 reference) they are left unchanged.

#include <cstddef>
#include <cstdint>

namespace gta6rt::isa {

enum class Sse4aOp : uint8_t { kNone, kExtrqImm, kExtrqReg, kInsertqImm, kInsertqReg };

struct Sse4aInsn {
    Sse4aOp op = Sse4aOp::kNone;
    uint8_t dst = 0;     // xmm index 0-15
    uint8_t src = 0;     // xmm index (register forms and insertq imm)
    uint8_t len = 0;     // imm forms: field length (0 means 64)
    uint8_t idx = 0;     // imm forms: bit index
    uint8_t length = 0;  // instruction bytes
};

// Decodes [66|F2] [REX] 0F 78/79 ModRM(mod=11) [ib ib]. Other legacy prefixes are rejected.
bool sse4a_decode(const uint8_t* p, size_t avail, Sse4aInsn* out);

uint64_t sse4a_extrq(uint64_t v, unsigned len, unsigned idx);
uint64_t sse4a_insertq(uint64_t dst, uint64_t src, unsigned len, unsigned idx);

// xmm: 16 registers as {lo, hi} qword pairs.
void sse4a_execute(const Sse4aInsn& in, uint64_t (*xmm)[2]);

}  // namespace gta6rt::isa
