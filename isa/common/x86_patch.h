#pragma once

// Load-time rewriting of guest x86-64 code: stub arena within rel32 reach, a minimal VEX/ModRM codec, the
// SSE4a and vpsrlvq/vpsllvq replacement stubs, int3 trap sites for cpuid/xgetbv, and the __chkstk patch.
// Everything here only writes x86 bytes, so it builds and is unit-tested on both host arches.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace gta6rt::isa {

struct X86Mem {
    bool rip = false;
    int base = -1;   // 0-15, -1 none
    int index = -1;  // 0-15, -1 none
    int scale = 0;   // log2
    int64_t disp = 0;
    uint64_t rip_target = 0;  // absolute address when rip
};

struct VexInsn {
    uint8_t map = 0;  // 1 = 0F, 2 = 0F38, 3 = 0F3A
    uint8_t w = 0, l = 0, pp = 0;
    uint8_t opcode = 0;
    uint8_t reg = 0;   // ModRM.reg with VEX.R
    uint8_t vvvv = 0;  // decoded (not inverted)
    bool rm_is_reg = false;
    uint8_t rm = 0;  // register when rm_is_reg
    X86Mem mem;
    uint8_t length = 0;
};

// Decodes a VEX-encoded instruction without immediate at address `ip` (for rip-relative targets).
bool vex_decode(const uint8_t* p, size_t avail, uint64_t ip, VexInsn* out);

class X86Emitter {
public:
    explicit X86Emitter(uint64_t base_address) : base_(base_address) {}
    uint64_t here() const { return base_ + buf_.size(); }
    const std::vector<uint8_t>& bytes() const { return buf_; }
    void raw(std::initializer_list<uint8_t> b) { buf_.insert(buf_.end(), b); }
    void u8(uint8_t v) { buf_.push_back(v); }
    void u32(uint32_t v);
    void u64(uint64_t v);
    // VEX3: reg field, vvvv, rm register or memory.
    void vex_rr(uint8_t map, uint8_t pp, uint8_t w, uint8_t l, uint8_t op, uint8_t reg, uint8_t vvvv, uint8_t rm,
                int imm = -1);
    void vex_rm(uint8_t map, uint8_t pp, uint8_t w, uint8_t l, uint8_t op, uint8_t reg, uint8_t vvvv, const X86Mem& m,
                int imm = -1);
    // Legacy SSE movlps with [rsp + d8]: store (0F 13) or load (0F 12).
    void movlps_rsp(bool store, uint8_t xmm, int8_t d8);

private:
    void modrm_mem(uint8_t reg, const X86Mem& m, size_t insn_start, int imm_bytes);
    uint64_t base_;
    std::vector<uint8_t> buf_;
};

// Stub arena: executable memory within +-2 GiB of `near`. Returns 0 on failure. The bytes are copied in.
uint64_t arena_put(uint64_t near, const std::vector<uint8_t>& code, uint64_t* out_zero32 = nullptr);
// Reserve `size` bytes in an arena near `near` (to emit position-dependent code there). 0 on failure.
uint64_t arena_reserve(uint64_t near, size_t size, uint64_t* out_zero32);
// Arena pages stay executable for their whole life (RWX, or RX plus a separate RW alias), so writing a new stub
// never disturbs another thread running code on the same page. Fails if [at, at+n) is not inside one arena.
bool arena_write(uint64_t at, const std::vector<uint8_t>& code);
bool arena_write(uint64_t at, const void* code, size_t n);
// Signal-safe. Under Rosetta, once it has raised SIGILL for an instruction on a page (e.g. SSE4a), writes to that
// RWX page fault until its protection is set again; re-asserting RWX before every write instead races with Rosetta
// when another thread runs code on the page (measured: the writer faults). So the fault handler calls this for a
// write fault: if `addr` is in an RWX arena it re-asserts RWX on that page and the write can be retried.
bool arena_retry_write_fault(uint64_t addr);
uint64_t arena_fault_retries();
// True when arenas are mapped RWX (x86_64); false when they use the RW alias (arm64, or RWX refused).
bool arena_is_rwx();

// Stub generators; `stub_addr` is where the code will live.
std::vector<uint8_t> make_sse4a_stub(uint64_t stub_addr, uint8_t op, uint8_t dst, uint8_t src, uint8_t len,
                                     uint8_t idx);
std::vector<uint8_t> make_vshift_stub(uint64_t stub_addr, const VexInsn& in, uint64_t zero32_addr);

// MONITORX/MWAITX wait windows: `len` bytes at `at` that end with the instruction (at `insn_off`) and start with
// position-independent setup the stub replays verbatim; the window becomes `call stub` + NOPs. The stub keeps every
// GPR, xmm0-5 and the flags, and calls `host_entry` (a NATIVE thunk) with rcx = rAX for MONITORX, or rcx = ECX,
// rdx = EAX, r8 = EBX for MWAITX. Entries come from the backend's provider; without one the windows stay NOPs.
enum class ZenWaitKind : uint8_t { MonitorX = 1, MWaitX = 2 };
using ZenWaitEntryFn = uint64_t (*)(ZenWaitKind);
void set_zen_wait_provider(ZenWaitEntryFn fn);
std::vector<uint8_t> make_zen_wait_stub(uint64_t stub_addr, const uint8_t* prefix, size_t prefix_len, ZenWaitKind kind,
                                        uint64_t host_entry);
int patch_zen_wait_window(uint64_t at, const uint8_t* expect, size_t len, size_t insn_off, ZenWaitKind kind);
struct ZenWaitStats {
    uint64_t monitors, waits, woken, timeouts, unarmed, sync_errors;
};
ZenWaitStats zen_wait_stats();  // backend (zeros where the backend has no emulation)

// vpsrlvq/vpsllvq run-time check of the static verdicts (GTA6RT_ISA_VSHIFT_CHECK=1): the site becomes a call to a
// stub that counts executions, counts executions with any count lane >= 2^32 (the static analysis says that never
// happens at a SAFE site) and keeps the last such count, then computes the x86 result as the vshift stub does.
struct VshiftCheckCounters {
    uint64_t executed;
    uint64_t high_count;   // executions with a lane whose high dword is nonzero
    uint64_t last_bad[4];  // the last such count vector (low 128 bits for the xmm form)
};
std::vector<uint8_t> make_vshift_check_stub(uint64_t stub_addr, const VexInsn& in, uint64_t zero32_addr,
                                            VshiftCheckCounters* counters);
// Patches one site with the check stub (`rva` and `tool_safe` label it in the logged report); `out` gets its counters.
int patch_vshift_check_site(uint64_t site, uint32_t rva, bool tool_safe, VshiftCheckCounters** out);
bool vshift_check_enabled();

// Write bytes into (possibly read-only, executable) memory and flush the instruction cache. Arena addresses go
// through arena_write. Other pages are made writable for the duration (RWX if allowed, else RW, which removes
// execute permission): only for code no other thread is executing, e.g. an image before it runs.
int code_write(uint64_t addr, const void* bytes, size_t n);

}  // namespace gta6rt::isa
