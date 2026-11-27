#include "x86_patch.h"

#include "isa_internal.h"
#include "sse4a.h"

#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <iterator>
#include <cstring>
#include <mutex>
#include <string>

#include <os/os_sync_wait_on_address.h>
#include <pthread.h>

namespace gta6rt::isa {

std::atomic<uint64_t> g_trap_count{0};

// ---- VEX decode ---------------------------------------------------------------------------------------------

bool vex_decode(const uint8_t* p, size_t avail, uint64_t ip, VexInsn* out) {
    if (avail < 4) return false;
    VexInsn v;
    size_t i;
    unsigned R, X = 0, B = 0;
    if (p[0] == 0xC4) {
        R = !(p[1] & 0x80);
        X = !(p[1] & 0x40);
        B = !(p[1] & 0x20);
        v.map = p[1] & 0x1F;
        v.w = p[2] >> 7;
        v.vvvv = (~p[2] >> 3) & 15;
        v.l = (p[2] >> 2) & 1;
        v.pp = p[2] & 3;
        i = 3;
    } else if (p[0] == 0xC5) {
        R = !(p[1] & 0x80);
        v.map = 1;
        v.vvvv = (~p[1] >> 3) & 15;
        v.l = (p[1] >> 2) & 1;
        v.pp = p[1] & 3;
        i = 2;
    } else {
        return false;
    }
    if (i + 2 > avail) return false;
    v.opcode = p[i++];
    const uint8_t modrm = p[i++];
    const unsigned mod = modrm >> 6, rm = modrm & 7;
    v.reg = static_cast<uint8_t>(((modrm >> 3) & 7) | (R << 3));
    if (mod == 3) {
        v.rm_is_reg = true;
        v.rm = static_cast<uint8_t>(rm | (B << 3));
    } else {
        int dispsz = mod == 1 ? 1 : (mod == 2 ? 4 : 0);
        if (rm == 4) {
            if (i >= avail) return false;
            const uint8_t sib = p[i++];
            v.mem.scale = sib >> 6;
            const unsigned idx = ((sib >> 3) & 7) | (X << 3);
            v.mem.index = idx == 4 ? -1 : static_cast<int>(idx);
            if ((sib & 7) == 5 && mod == 0) {
                v.mem.base = -1;
                dispsz = 4;
            } else {
                v.mem.base = static_cast<int>((sib & 7) | (B << 3));
            }
        } else if (rm == 5 && mod == 0) {
            v.mem.rip = true;
            dispsz = 4;
        } else {
            v.mem.base = static_cast<int>(rm | (B << 3));
        }
        if (i + dispsz > avail) return false;
        if (dispsz == 1) v.mem.disp = static_cast<int8_t>(p[i]);
        if (dispsz == 4) {
            int32_t d;
            memcpy(&d, p + i, 4);
            v.mem.disp = d;
        }
        i += dispsz;
    }
    v.length = static_cast<uint8_t>(i);
    if (v.mem.rip) v.mem.rip_target = ip + i + static_cast<uint64_t>(v.mem.disp);
    *out = v;
    return true;
}

// ---- emitter ----------------------------------------------------------------------------------------------

void X86Emitter::u32(uint32_t v) {
    for (int k = 0; k < 4; ++k) buf_.push_back(static_cast<uint8_t>(v >> (8 * k)));
}

void X86Emitter::u64(uint64_t v) {
    for (int k = 0; k < 8; ++k) buf_.push_back(static_cast<uint8_t>(v >> (8 * k)));
}

static uint8_t vex_b1(unsigned R, unsigned X, unsigned B, uint8_t map) {
    return static_cast<uint8_t>(((~R & 1) << 7) | ((~X & 1) << 6) | ((~B & 1) << 5) | (map & 0x1F));
}

static uint8_t vex_b2(uint8_t w, uint8_t vvvv, uint8_t l, uint8_t pp) {
    return static_cast<uint8_t>((w << 7) | (((~vvvv) & 15) << 3) | ((l & 1) << 2) | (pp & 3));
}

void X86Emitter::vex_rr(uint8_t map, uint8_t pp, uint8_t w, uint8_t l, uint8_t op, uint8_t reg, uint8_t vvvv,
                        uint8_t rm, int imm) {
    raw({0xC4, vex_b1(reg >> 3, 0, rm >> 3, map), vex_b2(w, vvvv, l, pp), op,
         static_cast<uint8_t>(0xC0 | ((reg & 7) << 3) | (rm & 7))});
    if (imm >= 0) u8(static_cast<uint8_t>(imm));
}

void X86Emitter::vex_rm(uint8_t map, uint8_t pp, uint8_t w, uint8_t l, uint8_t op, uint8_t reg, uint8_t vvvv,
                        const X86Mem& m, int imm) {
    const size_t start = buf_.size();
    const unsigned X = m.index >= 0 ? (m.index >> 3) : 0;
    const unsigned B = (!m.rip && m.base >= 0) ? (m.base >> 3) : 0;
    raw({0xC4, vex_b1(reg >> 3, X, B, map), vex_b2(w, vvvv, l, pp), op});
    modrm_mem(reg, m, start, imm >= 0 ? 1 : 0);
    if (imm >= 0) u8(static_cast<uint8_t>(imm));
}

void X86Emitter::modrm_mem(uint8_t reg, const X86Mem& m, size_t, int imm_bytes) {
    const uint8_t r = (reg & 7) << 3;
    if (m.rip) {
        u8(0x05 | r);
        const uint64_t end = base_ + buf_.size() + 4 + imm_bytes;
        u32(static_cast<uint32_t>(m.rip_target - end));
        return;
    }
    if (m.base < 0) {
        u8(0x04 | r);
        u8(static_cast<uint8_t>((m.scale << 6) | ((m.index < 0 ? 4 : (m.index & 7)) << 3) | 5));
        u32(static_cast<uint32_t>(m.disp));
        return;
    }
    if (m.index < 0 && (m.base & 7) != 4) {
        u8(0x80 | r | (m.base & 7));
    } else {
        u8(0x84 | r);
        u8(static_cast<uint8_t>((m.scale << 6) | ((m.index < 0 ? 4 : (m.index & 7)) << 3) | (m.base & 7)));
    }
    u32(static_cast<uint32_t>(m.disp));
}

void X86Emitter::movlps_rsp(bool store, uint8_t xmm, int8_t d8) {
    if (xmm >= 8) u8(0x44);
    raw({0x0F, static_cast<uint8_t>(store ? 0x13 : 0x12), static_cast<uint8_t>(0x44 | ((xmm & 7) << 3)), 0x24,
         static_cast<uint8_t>(d8)});
}

// ---- code memory ------------------------------------------------------------------------------------------

static int page_prot(uint64_t addr, vm_prot_t* out) {
    mach_vm_address_t a = addr;
    mach_vm_size_t sz = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info),
                       &cnt, &obj) != KERN_SUCCESS)
        return -1;
    if (a > addr) return -1;
    *out = info.protection;
    return 0;
}

static int to_posix(vm_prot_t p) {
    return ((p & VM_PROT_READ) ? PROT_READ : 0) | ((p & VM_PROT_WRITE) ? PROT_WRITE : 0) |
           ((p & VM_PROT_EXECUTE) ? PROT_EXEC : 0);
}

namespace {

// wdelta: offset from the executable view to a writable view of the same pages (0 when the arena is RWX).
struct Arena {
    uint64_t base, size, used, zero32;
    int64_t wdelta;
};

std::mutex g_arena_mu;
std::vector<Arena> g_arenas;
constexpr uint64_t kArenaSize = 1 << 20;
constexpr int kMaxRwxArenas = 256;
std::atomic<uint64_t> g_rwx_base[kMaxRwxArenas];
std::atomic<int> g_n_rwx{0};
std::atomic<uint64_t> g_fault_retries{0};
constexpr uint64_t kReach = (1ull << 31) - (64ull << 20);

bool in_reach(uint64_t a, uint64_t b) {
    return (a > b ? a - b : b - a) < kReach;
}

// 1: written, 0: [at, at+n) is not inside one arena, -1: error.
int arena_store(uint64_t at, const void* code, size_t n) {
    int64_t wd = 0;
    {
        std::lock_guard<std::mutex> lk(g_arena_mu);
        const Arena* hit = nullptr;
        for (const Arena& a : g_arenas)
            if (at >= a.base && at + n <= a.base + a.size) hit = &a;
        if (!hit) return 0;
        wd = hit->wdelta;
    }
    memcpy(reinterpret_cast<void*>(at + static_cast<uint64_t>(wd)), code, n);
    sys_icache_invalidate(reinterpret_cast<void*>(at), n);
    return 1;
}

// Maps a fresh arena at `hint` that stays executable for its whole life: RWX where the platform allows it, else an
// RX view plus a second RW view of the same pages (mach_vm_remap, shared). Never removes execute permission.
// The alias is only correct for bytes that have not run yet: under Rosetta a write through it to code that has
// already been translated is not seen (measured: 196 of 200 rewrites stale). Arena bytes are written once.
bool arena_map(uint64_t hint, uint64_t* base, int64_t* wdelta) {
#if defined(__x86_64__)
    {
        void* p = mmap(reinterpret_cast<void*>(hint), kArenaSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p != MAP_FAILED) {
            *base = reinterpret_cast<uint64_t>(p);
            *wdelta = 0;
            return true;
        }
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) log_line("arena: RWX mapping refused (errno %d), using an RW alias", errno);
    }
#endif
    void* p = mmap(reinterpret_cast<void*>(hint), kArenaSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return false;
    mach_vm_address_t alias = 0;
    vm_prot_t cur = 0, max = 0;
    if (mach_vm_remap(mach_task_self(), &alias, kArenaSize, 0, VM_FLAGS_ANYWHERE, mach_task_self(),
                      reinterpret_cast<mach_vm_address_t>(p), FALSE, &cur, &max, VM_INHERIT_NONE) != KERN_SUCCESS ||
        !(cur & VM_PROT_WRITE)) {
        munmap(p, kArenaSize);
        return false;
    }
    if (mprotect(p, kArenaSize, PROT_READ | PROT_EXEC) != 0) {
        mach_vm_deallocate(mach_task_self(), alias, kArenaSize);
        munmap(p, kArenaSize);
        return false;
    }
    *base = reinterpret_cast<uint64_t>(p);
    *wdelta = static_cast<int64_t>(alias - *base);
    return true;
}

}  // namespace

int code_write(uint64_t addr, const void* bytes, size_t n) {
    if (const int r = arena_store(addr, bytes, n)) return r > 0 ? 0 : -1;
    const uint64_t pg = static_cast<uint64_t>(getpagesize());
    const uint8_t* src = static_cast<const uint8_t*>(bytes);
    uint64_t cur = addr;
    while (cur < addr + n) {
        const uint64_t page = cur & ~(pg - 1);
        const uint64_t chunk_end = std::min(addr + n, page + pg);
        vm_prot_t prot;
        if (page_prot(page, &prot) != 0) return -1;
        const bool writable = prot & VM_PROT_WRITE;
        if (!writable) {
            // Keep execute permission while writing where the platform allows RWX, so other threads running
            // code on this page do not fault. The bytes themselves are still not written atomically.
            const bool rwx = (prot & VM_PROT_EXECUTE) &&
                             mprotect(reinterpret_cast<void*>(page), pg, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
            if (!rwx && mprotect(reinterpret_cast<void*>(page), pg, PROT_READ | PROT_WRITE) != 0) return -1;
        }
        memcpy(reinterpret_cast<void*>(cur), src + (cur - addr), chunk_end - cur);
        if (!writable && mprotect(reinterpret_cast<void*>(page), pg, to_posix(prot)) != 0) return -1;
        cur = chunk_end;
    }
    sys_icache_invalidate(reinterpret_cast<void*>(addr), n);
    return 0;
}

uint64_t arena_reserve(uint64_t near, size_t size, uint64_t* out_zero32) {
    size = (size + 15) & ~static_cast<size_t>(15);
    std::lock_guard<std::mutex> lk(g_arena_mu);
    for (Arena& a : g_arenas) {
        if (in_reach(a.base, near) && in_reach(a.base + a.size, near) && a.used + size <= a.size) {
            const uint64_t at = a.base + a.used;
            a.used += size;
            if (out_zero32) *out_zero32 = a.zero32;
            return at;
        }
    }
    for (int k = 1; k <= 120; ++k) {
        for (int sign : {-1, 1}) {
            const int64_t off = static_cast<int64_t>(k) * (16ll << 20) * sign;
            const uint64_t hint = (near + static_cast<uint64_t>(off)) & ~(kArenaSize - 1);
            uint64_t b = 0;
            int64_t wd = 0;
            if (!arena_map(hint, &b, &wd)) continue;
            if (!in_reach(b, near) || !in_reach(b + kArenaSize, near)) {
                if (wd) mach_vm_deallocate(mach_task_self(), b + static_cast<uint64_t>(wd), kArenaSize);
                munmap(reinterpret_cast<void*>(b), kArenaSize);
                continue;
            }
            Arena a{b, kArenaSize, 64 + size, b, wd};
            g_arenas.push_back(a);
            const int k = g_n_rwx.load(std::memory_order_relaxed);
            if (wd == 0 && k < kMaxRwxArenas) {
                g_rwx_base[k].store(b, std::memory_order_relaxed);
                g_n_rwx.store(k + 1, std::memory_order_release);
            }
            if (out_zero32) *out_zero32 = a.zero32;
            return b + 64;
        }
    }
    return 0;
}

bool arena_write(uint64_t at, const void* code, size_t n) {
    return arena_store(at, code, n) > 0;
}

bool arena_write(uint64_t at, const std::vector<uint8_t>& code) {
    return arena_write(at, code.data(), code.size());
}

bool arena_retry_write_fault(uint64_t addr) {
    const int n = g_n_rwx.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        const uint64_t b = g_rwx_base[i].load(std::memory_order_relaxed);
        if (addr >= b && addr < b + kArenaSize) {
            const uint64_t pg = static_cast<uint64_t>(getpagesize());
            g_fault_retries.fetch_add(1, std::memory_order_relaxed);
            return mprotect(reinterpret_cast<void*>(addr & ~(pg - 1)), pg, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
        }
    }
    return false;
}

uint64_t arena_fault_retries() {
    return g_fault_retries.load(std::memory_order_relaxed);
}

bool arena_is_rwx() {
    std::lock_guard<std::mutex> lk(g_arena_mu);
    return !g_arenas.empty() && g_arenas.front().wdelta == 0;
}

uint64_t arena_put(uint64_t near, const std::vector<uint8_t>& code, uint64_t* out_zero32) {
    const uint64_t at = arena_reserve(near, code.size(), out_zero32);
    if (!at || !arena_write(at, code)) return 0;
    return at;
}

// ---- stubs ------------------------------------------------------------------------------------------------

std::vector<uint8_t> make_sse4a_stub(uint64_t stub_addr, uint8_t op, uint8_t dst, uint8_t src, uint8_t len,
                                     uint8_t idx) {
    X86Emitter e(stub_addr);
    const unsigned flen = (len & 63) ? (len & 63) : 64;
    const unsigned sh = idx & 63;
    const bool insert = op == static_cast<uint8_t>(Sse4aOp::kInsertqImm);
    e.raw({0x9C, 0x50});                    // pushfq; push rax
    if (insert) e.raw({0x51, 0x52});        // push rcx; push rdx
    e.raw({0x48, 0x8D, 0x64, 0x24, 0xF0});  // lea rsp, [rsp-16]
    e.movlps_rsp(true, dst, 0);
    if (insert) e.movlps_rsp(true, src, 8);
    e.raw({0x48, 0x8B, 0x04, 0x24});  // mov rax, [rsp]
    if (insert) {
        e.raw({0x48, 0x8B, 0x54, 0x24, 0x08});  // mov rdx, [rsp+8]
        if (flen < 64) {
            e.raw({0x48, 0xC1, 0xE2, static_cast<uint8_t>(64 - flen)});  // shl rdx, 64-len
            e.raw({0x48, 0xC1, 0xEA, static_cast<uint8_t>(64 - flen)});  // shr rdx, 64-len
        }
        if (sh) e.raw({0x48, 0xC1, 0xE2, static_cast<uint8_t>(sh)});  // shl rdx, idx
        const uint64_t m = flen == 64 ? ~0ull : ((1ull << flen) - 1);
        e.raw({0x48, 0xB9});
        e.u64(~(m << sh));                 // mov rcx, ~(mask << idx)
        e.raw({0x48, 0x21, 0xC8});         // and rax, rcx
        e.raw({0x48, 0x09, 0xD0});         // or rax, rdx
    } else {
        if (sh) e.raw({0x48, 0xC1, 0xE8, static_cast<uint8_t>(sh)});  // shr rax, idx
        if (flen < 64) {
            e.raw({0x48, 0xC1, 0xE0, static_cast<uint8_t>(64 - flen)});  // shl rax, 64-len
            e.raw({0x48, 0xC1, 0xE8, static_cast<uint8_t>(64 - flen)});  // shr rax, 64-len
        }
    }
    e.raw({0x48, 0x89, 0x04, 0x24});  // mov [rsp], rax
    e.movlps_rsp(false, dst, 0);
    e.raw({0x48, 0x8D, 0x64, 0x24, 0x10});  // lea rsp, [rsp+16]
    if (insert) e.raw({0x5A, 0x59});        // pop rdx; pop rcx
    e.raw({0x58, 0x9D, 0xC3});              // pop rax; popfq; ret
    return e.bytes();
}

std::vector<uint8_t> make_vshift_stub(uint64_t stub_addr, const VexInsn& in, uint64_t zero32_addr) {
    bool used[16] = {};
    used[in.reg] = used[in.vvvv] = true;
    if (in.rm_is_reg) used[in.rm] = true;
    uint8_t t[2], n = 0;
    for (uint8_t r = 15; n < 2; --r)
        if (!used[r]) t[n++] = r;
    const uint8_t t1 = t[0], t2 = t[1];
    X86Emitter e(stub_addr);
    X86Mem s0, s1;
    s0.base = 4;
    s1.base = 4;
    s1.disp = 0x20;
    e.raw({0x48, 0x8D, 0x64, 0x24, 0xC0});        // lea rsp, [rsp-0x40]
    e.vex_rm(1, 2, 0, 1, 0x7F, t1, 0, s0);        // vmovdqu [rsp], ymm_t1
    e.vex_rm(1, 2, 0, 1, 0x7F, t2, 0, s1);        // vmovdqu [rsp+0x20], ymm_t2
    if (in.rm_is_reg) {
        e.vex_rr(1, 1, 0, in.l, 0x6F, t1, 0, in.rm);  // vmovdqa t1, count
    } else {
        X86Mem m = in.mem;
        if (!m.rip && m.base == 4) m.disp += 8 + 0x40;  // call pushed 8, we moved rsp by 0x40
        e.vex_rm(1, 2, 0, in.l, 0x6F, t1, 0, m);        // vmovdqu t1, [count]
    }
    e.vex_rr(1, 1, 0, in.l, 0x73, 2, t2, t1, 6);  // vpsrlq t2, t1, 6
    X86Mem z;
    z.rip = true;
    z.rip_target = zero32_addr;
    e.vex_rm(2, 1, 0, in.l, 0x29, t2, t2, z);              // vpcmpeqq t2, t2, [zero]: lanes with count < 64
    e.vex_rr(2, 1, 1, in.l, in.opcode, in.reg, in.vvvv, t1);  // vpsrlvq/vpsllvq dst, src, t1
    e.vex_rr(1, 1, 0, in.l, 0xDB, in.reg, in.reg, t2);     // vpand dst, dst, t2
    e.vex_rm(1, 2, 0, 1, 0x6F, t1, 0, s0);                 // vmovdqu ymm_t1, [rsp]
    e.vex_rm(1, 2, 0, 1, 0x6F, t2, 0, s1);                 // vmovdqu ymm_t2, [rsp+0x20]
    e.raw({0x48, 0x8D, 0x64, 0x24, 0x40, 0xC3});           // lea rsp, [rsp+0x40]; ret
    return e.bytes();
}

std::vector<uint8_t> make_vshift_check_stub(uint64_t stub_addr, const VexInsn& in, uint64_t zero32_addr,
                                            VshiftCheckCounters* counters) {
    bool used[16] = {};
    used[in.reg] = used[in.vvvv] = true;
    if (in.rm_is_reg) used[in.rm] = true;
    uint8_t t[2], n = 0;
    for (uint8_t r = 15; n < 2; --r)
        if (!used[r]) t[n++] = r;
    const uint8_t t1 = t[0], t2 = t[1];
    X86Emitter e(stub_addr);
    X86Mem s0, s1, bad;
    s0.base = 4;
    s1.base = 4;
    s1.disp = 0x20;
    bad.base = 0;
    bad.disp = 0x10;
    e.raw({0x9C, 0x50});                          // pushfq; push rax
    e.raw({0x48, 0x8D, 0x64, 0x24, 0xC0});        // lea rsp, [rsp-0x40]
    e.vex_rm(1, 2, 0, 1, 0x7F, t1, 0, s0);        // vmovdqu [rsp], ymm_t1
    e.vex_rm(1, 2, 0, 1, 0x7F, t2, 0, s1);        // vmovdqu [rsp+0x20], ymm_t2
    if (in.rm_is_reg) {
        e.vex_rr(1, 1, 0, in.l, 0x6F, t1, 0, in.rm);  // vmovdqa t1, count
    } else {
        X86Mem m = in.mem;
        if (!m.rip && m.base == 4) m.disp += 8 + 16 + 0x40;  // call, pushfq + push rax, the spill area
        e.vex_rm(1, 2, 0, in.l, 0x6F, t1, 0, m);              // vmovdqu t1, [count]
    }
    e.raw({0x48, 0xB8});
    e.u64(reinterpret_cast<uint64_t>(counters));  // mov rax, counters
    e.raw({0xF0, 0x48, 0xFF, 0x00});              // lock inc qword [rax]
    e.vex_rr(1, 1, 0, in.l, 0x73, 2, t2, t1, 32);  // vpsrlq t2, t1, 32
    e.vex_rr(2, 1, 0, in.l, 0x17, t2, 0, t2);      // vptest t2, t2
    X86Emitter b(0);
    b.raw({0xF0, 0x48, 0xFF, 0x40, 0x08});      // lock inc qword [rax+8]
    b.vex_rm(1, 2, 0, in.l, 0x7F, t1, 0, bad);  // vmovdqu [rax+0x10], t1
    e.raw({0x74, static_cast<uint8_t>(b.bytes().size())});  // jz past the record
    for (uint8_t v : b.bytes()) e.u8(v);
    e.vex_rr(1, 1, 0, in.l, 0x73, 2, t2, t1, 6);  // vpsrlq t2, t1, 6
    X86Mem z;
    z.rip = true;
    z.rip_target = zero32_addr;
    e.vex_rm(2, 1, 0, in.l, 0x29, t2, t2, z);                 // vpcmpeqq t2, t2, [zero]: lanes with count < 64
    e.vex_rr(2, 1, 1, in.l, in.opcode, in.reg, in.vvvv, t1);  // vpsrlvq/vpsllvq dst, src, t1
    e.vex_rr(1, 1, 0, in.l, 0xDB, in.reg, in.reg, t2);        // vpand dst, dst, t2
    e.vex_rm(1, 2, 0, 1, 0x6F, t1, 0, s0);                    // vmovdqu ymm_t1, [rsp]
    e.vex_rm(1, 2, 0, 1, 0x6F, t2, 0, s1);                    // vmovdqu ymm_t2, [rsp+0x20]
    e.raw({0x48, 0x8D, 0x64, 0x24, 0x40, 0x58, 0x9D, 0xC3});  // lea rsp, [rsp+0x40]; pop rax; popfq; ret
    return e.bytes();
}

std::vector<uint8_t> make_zen_wait_stub(uint64_t stub_addr, const uint8_t* prefix, size_t prefix_len, ZenWaitKind kind,
                                        uint64_t host_entry) {
    X86Emitter e(stub_addr);
    for (size_t i = 0; i < prefix_len; ++i) e.u8(prefix[i]);
    e.raw({0x9C, 0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});  // pushfq; rax rcx rdx r8-r11
    e.raw({0x55, 0x48, 0x89, 0xE5, 0x48, 0x83, 0xE4, 0xF0});                          // push rbp; mov rbp, rsp; and rsp, -16
    e.raw({0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00});                                // sub rsp, 0x80
    for (uint8_t x = 0; x < 6; ++x)
        e.raw({0xF3, 0x0F, 0x7F, static_cast<uint8_t>(0x44 | (x << 3)), 0x24, static_cast<uint8_t>(0x20 + 16 * x)});
    if (kind == ZenWaitKind::MonitorX)
        e.raw({0x48, 0x89, 0xC1});  // mov rcx, rax
    else
        e.raw({0x48, 0x89, 0xC2, 0x49, 0x89, 0xD8, 0x89, 0xC9});  // mov rdx, rax; mov r8, rbx; mov ecx, ecx
    e.raw({0x49, 0xBB});
    e.u64(host_entry);           // mov r11, entry
    e.raw({0x41, 0xFF, 0xD3});  // call r11
    for (uint8_t x = 0; x < 6; ++x)
        e.raw({0xF3, 0x0F, 0x6F, static_cast<uint8_t>(0x44 | (x << 3)), 0x24, static_cast<uint8_t>(0x20 + 16 * x)});
    e.raw({0x48, 0x89, 0xEC, 0x5D});                                            // mov rsp, rbp; pop rbp
    e.raw({0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58});  // pop r11-r8, rdx, rcx, rax
    e.raw({0x9D, 0xC3});                                                        // popfq; ret
    return e.bytes();
}

// ---- trap sites -------------------------------------------------------------------------------------------

namespace {
struct TrapSite {
    uint64_t site;
    uint8_t kind, len;
};
constexpr int kMaxTraps = 1024;
TrapSite g_traps[kMaxTraps];
std::atomic<int> g_ntraps{0};
std::mutex g_trap_mu;

bool trap_add(uint64_t site, TrapKind kind, uint8_t len) {
    std::lock_guard<std::mutex> lk(g_trap_mu);
    const int n = g_ntraps.load();
    for (int i = 0; i < n; ++i)
        if (g_traps[i].site == site) return true;
    if (n >= kMaxTraps) return false;
    g_traps[n] = TrapSite{site, kind, len};
    g_ntraps.store(n + 1, std::memory_order_release);
    return true;
}
}  // namespace

TrapKind trap_lookup(uint64_t site, uint8_t* out_len) {
    const int n = g_ntraps.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        if (g_traps[i].site == site) {
            *out_len = g_traps[i].len;
            return static_cast<TrapKind>(g_traps[i].kind);
        }
    }
    return kTrapNone;
}

static bool g_cpuid_trapped = false;

bool cpuid_sites_trapped() {
    return g_cpuid_trapped;
}

// ---- __chkstk ------------------------------------------------------------------------------------------------

// MSVC __chkstk as found in the title (S0.1 review): reads gs:[0x10] = StackLimit, which is libc's TSD slot 2
// under SHARE. The function has no relocations or rip-relative operands, so it matches byte for byte.
static const uint8_t kChkstk[78] = {
    0x48, 0x83, 0xec, 0x10, 0x4c, 0x89, 0x14, 0x24, 0x4c, 0x89, 0x5c, 0x24, 0x08, 0x4d, 0x33, 0xdb, 0x4c, 0x8d, 0x54, 0x24,
    0x18, 0x4c, 0x2b, 0xd0, 0x4d, 0x0f, 0x42, 0xd3, 0x65, 0x4c, 0x8b, 0x1c, 0x25, 0x10, 0x00, 0x00, 0x00, 0x4d, 0x3b, 0xd3,
    0x73, 0x16, 0x66, 0x41, 0x81, 0xe2, 0x00, 0xf0, 0x4d, 0x8d, 0x9b, 0x00, 0xf0, 0xff, 0xff, 0x41, 0xc6, 0x03, 0x00, 0x4d,
    0x3b, 0xd3, 0x75, 0xf0, 0x4c, 0x8b, 0x14, 0x24, 0x4c, 0x8b, 0x5c, 0x24, 0x08, 0x48, 0x83, 0xc4, 0x10, 0xc3};

static bool chkstk_matches(const uint8_t* p) {
    return (p[0] == kChkstk[0] || p[0] == 0xC3) && memcmp(p + 1, kChkstk + 1, sizeof kChkstk - 1) == 0;
}

struct TitleSite {
    uint32_t rva;
    uint8_t kind;
    uint8_t len;
    uint8_t bytes[15];
};
#include "title_sites.inc"

enum : uint8_t { kSiteChkstk = 1, kSiteSse4a = 2, kSiteVshiftPatch = 3, kSiteVshiftSafe = 4, kSiteCpuid = 5, kSiteXgetbv = 6 };

static bool readable(uint64_t addr, size_t n) {
    uint64_t cur = addr;
    while (cur < addr + n) {
        mach_vm_address_t a = cur;
        mach_vm_size_t sz = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64,
                           reinterpret_cast<vm_region_info_t>(&info), &cnt, &obj) != KERN_SUCCESS)
            return false;
        if (a > cur || !(info.protection & VM_PROT_READ)) return false;
        cur = a + sz;
    }
    return true;
}

}  // namespace gta6rt::isa

using namespace gta6rt::isa;

extern "C" {

gta6rt_status gta6rt_isa_patch_code(gta6rt_gva addr, const void* bytes, size_t size) {
    if (!addr || !bytes) return GTA6RT_E_INVALID_ARG;
    return code_write(addr, bytes, size) == 0 ? GTA6RT_OK : GTA6RT_E_ACCESS;
}

void gta6rt_isa_flush_icache(gta6rt_gva addr, size_t size) {
    sys_icache_invalidate(reinterpret_cast<void*>(addr), size);
}

gta6rt_gva gta6rt_isa_find_chkstk(gta6rt_gva base, size_t size) {
    if (size < sizeof kChkstk) return 0;
    // Walk readable regions only: a mapped image can have unmapped or PROT_NONE gaps.
    uint64_t cur = base;
    const uint64_t end = base + size;
    while (cur < end) {
        mach_vm_address_t a = cur;
        mach_vm_size_t sz = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64,
                           reinterpret_cast<vm_region_info_t>(&info), &cnt, &obj) != KERN_SUCCESS)
            break;
        if (a >= end) break;
        const uint64_t rb = std::max<uint64_t>(a, cur), re = std::min<uint64_t>(a + sz, end);
        if ((info.protection & VM_PROT_READ) && re - rb >= sizeof kChkstk) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(rb);
            const size_t n = re - rb;
            for (size_t i = 0; i + sizeof kChkstk <= n; ++i) {
                const void* hit = memmem(p + i, n - i, kChkstk + 1, 16);
                if (!hit) break;
                const size_t off = static_cast<const uint8_t*>(hit) - p;
                if (off >= 1 && off - 1 + sizeof kChkstk <= n && chkstk_matches(p + off - 1)) return rb + off - 1;
                i = off;
            }
        }
        cur = a + sz;
    }
    return 0;
}

gta6rt_status gta6rt_isa_patch_chkstk(gta6rt_gva fn) {
    if (!fn || !readable(fn, sizeof kChkstk)) return GTA6RT_E_INVALID_ARG;
    const auto* p = reinterpret_cast<const uint8_t*>(fn);
    if (!chkstk_matches(p)) return GTA6RT_E_BAD_FORMAT;
    if (p[0] == 0xC3) return GTA6RT_OK;
    const uint8_t ret = 0xC3;
    return code_write(fn, &ret, 1) == 0 ? GTA6RT_OK : GTA6RT_E_ACCESS;
}

static gta6rt_status write_call(gta6rt_gva site, size_t len, uint64_t stub) {
    const int64_t rel = static_cast<int64_t>(stub) - static_cast<int64_t>(site + 5);
    if (rel != static_cast<int32_t>(rel)) return GTA6RT_E_UNSUPPORTED;
    uint8_t b[16];
    b[0] = 0xE8;
    const int32_t r32 = static_cast<int32_t>(rel);
    memcpy(b + 1, &r32, 4);
    for (size_t i = 5; i < len; ++i) b[i] = 0x90;
    return code_write(site, b, len) == 0 ? GTA6RT_OK : GTA6RT_E_ACCESS;
}

gta6rt_status gta6rt_isa_patch_sse4a_site(gta6rt_gva site) {
    if (!site || !readable(site, 8)) return GTA6RT_E_INVALID_ARG;
    Sse4aInsn in;
    if (!sse4a_decode(reinterpret_cast<const uint8_t*>(site), 8, &in)) return GTA6RT_E_BAD_FORMAT;
    if (in.op != Sse4aOp::kInsertqImm && in.op != Sse4aOp::kExtrqImm) return GTA6RT_E_UNSUPPORTED;
    if (in.length < 5) return GTA6RT_E_UNSUPPORTED;
    const auto probe = make_sse4a_stub(0, static_cast<uint8_t>(in.op), in.dst, in.src, in.len, in.idx);
    const uint64_t at = arena_reserve(site, probe.size(), nullptr);
    if (!at) return GTA6RT_E_NO_MEMORY;
    const auto code = make_sse4a_stub(at, static_cast<uint8_t>(in.op), in.dst, in.src, in.len, in.idx);
    if (!arena_write(at, code)) return GTA6RT_E_ACCESS;
    return write_call(site, in.length, at);
}

gta6rt_status gta6rt_isa_patch_vshift_site(gta6rt_gva site) {
    if (!site || !readable(site, 15)) return GTA6RT_E_INVALID_ARG;
    VexInsn in;
    if (!vex_decode(reinterpret_cast<const uint8_t*>(site), 15, site, &in)) return GTA6RT_E_BAD_FORMAT;
    if (in.map != 2 || in.pp != 1 || in.w != 1 || (in.opcode != 0x45 && in.opcode != 0x47)) return GTA6RT_E_BAD_FORMAT;
    if (in.length < 5) return GTA6RT_E_UNSUPPORTED;
    uint64_t zero = 0;
    const auto probe = make_vshift_stub(0, in, 0);
    const uint64_t at = arena_reserve(site, probe.size(), &zero);
    if (!at) return GTA6RT_E_NO_MEMORY;
    if (in.mem.rip) {
        const int64_t d = static_cast<int64_t>(in.mem.rip_target) - static_cast<int64_t>(at);
        if (d != static_cast<int32_t>(d)) return GTA6RT_E_UNSUPPORTED;
    }
    const auto code = make_vshift_stub(at, in, zero);
    if (!arena_write(at, code)) return GTA6RT_E_ACCESS;
    return write_call(site, in.length, at);
}

}  // extern "C"

namespace gta6rt::isa {
namespace {

struct VshiftCheckSite {
    uint32_t rva;
    bool tool_safe;
    VshiftCheckCounters* c;
};
std::mutex g_vcheck_mu;
std::vector<VshiftCheckSite> g_vcheck;
std::atomic<bool> g_vcheck_thread{false};
}  // namespace

bool vshift_check_enabled() {
    static const bool on = [] {
        const char* v = getenv("GTA6RT_ISA_VSHIFT_CHECK");
        return v && v[0] == '1';
    }();
    return on;
}

namespace {

void vshift_check_log(bool sites) {
    std::vector<VshiftCheckSite> list;
    {
        std::lock_guard<std::mutex> lk(g_vcheck_mu);
        list = g_vcheck;
    }
    uint64_t exec = 0, bad = 0, ran = 0;
    for (const auto& v : list) {
        const uint64_t e = __atomic_load_n(&v.c->executed, __ATOMIC_RELAXED);
        const uint64_t h = __atomic_load_n(&v.c->high_count, __ATOMIC_RELAXED);
        exec += e;
        bad += h;
        ran += e != 0;
        if (h)
            log_line("vshift check: VIOLATION at rva 0x%x (%s verdict SAFE): %llu of %llu executions had a count lane >= "
                     "2^32; last count {%#llx, %#llx, %#llx, %#llx}",
                     v.rva, v.tool_safe ? "tool" : "manual", (unsigned long long)h, (unsigned long long)e,
                     (unsigned long long)v.c->last_bad[0], (unsigned long long)v.c->last_bad[1],
                     (unsigned long long)v.c->last_bad[2], (unsigned long long)v.c->last_bad[3]);
    }
    log_line("vshift check: %llu of %zu sites executed, %llu executions, %llu with a count lane >= 2^32",
             (unsigned long long)ran, list.size(), (unsigned long long)exec, (unsigned long long)bad);
    static std::atomic<uint64_t> listed_ran{0};
    if (!sites && listed_ran.exchange(ran) == ran) return;
    std::string line;
    for (const auto& v : list) {
        const uint64_t e = __atomic_load_n(&v.c->executed, __ATOMIC_RELAXED);
        if (!e) continue;
        char b[48];
        snprintf(b, sizeof b, " 0x%x%s:%llu", v.rva, v.tool_safe ? "" : "(m)", (unsigned long long)e);
        line += b;
        if (line.size() > 400) {
            log_line("vshift check: executed%s", line.c_str());
            line.clear();
        }
    }
    if (!line.empty()) log_line("vshift check: executed%s", line.c_str());
}

void* vshift_check_thread(void*) {
    for (unsigned tick = 1;; ++tick) {
        sleep(10);
        vshift_check_log(tick % 6 == 0);
    }
    return nullptr;
}

}  // namespace

gta6rt_status patch_vshift_check_site(uint64_t site, uint32_t rva, bool tool_safe, VshiftCheckCounters** out) {
    if (!site || !readable(site, 15)) return GTA6RT_E_INVALID_ARG;
    VexInsn in;
    if (!vex_decode(reinterpret_cast<const uint8_t*>(site), 15, site, &in)) return GTA6RT_E_BAD_FORMAT;
    if (in.map != 2 || in.pp != 1 || in.w != 1 || (in.opcode != 0x45 && in.opcode != 0x47)) return GTA6RT_E_BAD_FORMAT;
    if (in.length < 5) return GTA6RT_E_UNSUPPORTED;
    auto* c = new VshiftCheckCounters{};
    uint64_t zero = 0;
    const auto probe = make_vshift_check_stub(0, in, 0, c);
    const uint64_t at = arena_reserve(site, probe.size(), &zero);
    if (!at) return GTA6RT_E_NO_MEMORY;
    if (in.mem.rip) {
        const int64_t d = static_cast<int64_t>(in.mem.rip_target) - static_cast<int64_t>(at);
        if (d != static_cast<int32_t>(d)) return GTA6RT_E_UNSUPPORTED;
    }
    if (!arena_write(at, make_vshift_check_stub(at, in, zero, c))) return GTA6RT_E_ACCESS;
    {
        std::lock_guard<std::mutex> lk(g_vcheck_mu);
        g_vcheck.push_back({rva, tool_safe, c});
    }
    if (out) *out = c;
    if (!g_vcheck_thread.exchange(true)) {
        pthread_t t;
        if (pthread_create(&t, nullptr, vshift_check_thread, nullptr) == 0) pthread_detach(t);
        log_line("vshift check: on; counters are logged every 10 s");
    }
    return write_call(site, in.length, at);
}

}  // namespace gta6rt::isa

extern "C" {

void gta6rt_isa_vshift_check_report(void) {
    if (vshift_check_enabled()) vshift_check_log(true);
}

void gta6rt_isa_monitor_wake(const void* addr) {
    if (!addr || (reinterpret_cast<uintptr_t>(addr) & 7)) return;
    if (__builtin_available(macOS 14.4, *))
        os_sync_wake_by_address_all(const_cast<void*>(addr), 8, OS_SYNC_WAKE_BY_ADDRESS_NONE);
}

gta6rt_status gta6rt_isa_patch_cpuid_site(gta6rt_gva site) {
    if (!site || !readable(site, 3)) return GTA6RT_E_INVALID_ARG;
    const auto* p = reinterpret_cast<const uint8_t*>(site);
    TrapKind k;
    uint8_t len;
    if (p[0] == 0x0F && p[1] == 0xA2) {
        k = kTrapCpuid;
        len = 2;
    } else if (p[0] == 0x0F && p[1] == 0x01 && p[2] == 0xD0) {
        k = kTrapXgetbv;
        len = 3;
    } else {
        return GTA6RT_E_BAD_FORMAT;
    }
    if (!trap_add(site, k, len)) return GTA6RT_E_NO_MEMORY;
    const uint8_t b[3] = {0xCC, 0x90, 0x90};
    return code_write(site, b, len) == 0 ? GTA6RT_OK : GTA6RT_E_ACCESS;
}

uint64_t gta6rt_isa_trap_count(void) {
    return g_trap_count.load(std::memory_order_relaxed);
}

}  // extern "C"

namespace gta6rt::isa {

// Zen 2 instructions Rosetta raises SIGILL for, at sites of the known title (capstone census, tools/zen_sites.py).
// CLWB has no observable effect on this host and becomes a NOP of the same length.
namespace {
struct ZenSite {
    uint32_t rva;
    uint8_t len;
    uint8_t bytes[5];
};
const ZenSite kZenNopSites[] = {
    {0x4e4b680, 4, {0x66, 0x0f, 0xae, 0x31}},       {0x4ecc84a, 5, {0x66, 0x41, 0x0f, 0xae, 0x31}},
    {0x50f75f5, 5, {0x66, 0x41, 0x0f, 0xae, 0x30}}, {0x532f1b0, 4, {0x66, 0x0f, 0xae, 0x30}},
    {0x532f1f0, 4, {0x66, 0x0f, 0xae, 0x30}},       {0x532f230, 4, {0x66, 0x0f, 0xae, 0x30}},
    {0x532f280, 4, {0x66, 0x0f, 0xae, 0x30}},       {0x532f2ba, 4, {0x66, 0x0f, 0xae, 0x30}},
};

// The title's two wait loops in exe+0x4fb0ad0..0x4fb0e84 (a wait for a 64-bit value at [rdi] to pass rsi, with a
// callback in r12): `mov rax, rdi; xor ecx, ecx; xor edx, edx; monitorx`, re-check [rdi], then `mov ecx, 2;
// xor eax, eax; mwaitx` (timer on, EBX = max(2, 100.0 / [exe+0x9e4f5c4]) ticks), re-check, then Sleep(0) (loop 1,
// from 0x4fb0bd0) or Sleep(1) (loop 2, from 0x4fb0d14). Each window holds no relocation and no branch target other
// than its first byte (0x4fb0bd0 from 0x4fb0c2f, 0x4fb0bf9 from 0x4fb0c52). [rdi] is the D3D12 layer's fence mirror
// (fence slot 12, the completed value's address).
struct ZenWaitSite {
    uint32_t rva;
    uint8_t len, insn_off;
    ZenWaitKind kind;
    uint8_t bytes[10];
};
const ZenWaitSite kZenWaitSites[] = {
    {0x4fb0bd0, 10, 7, ZenWaitKind::MonitorX, {0x48, 0x89, 0xf8, 0x31, 0xc9, 0x31, 0xd2, 0x0f, 0x01, 0xfa}},
    {0x4fb0bf9, 10, 7, ZenWaitKind::MWaitX, {0xb9, 0x02, 0x00, 0x00, 0x00, 0x31, 0xc0, 0x0f, 0x01, 0xfb}},
    {0x4fb0d1d, 7, 4, ZenWaitKind::MonitorX, {0x31, 0xc9, 0x31, 0xd2, 0x0f, 0x01, 0xfa}},
    {0x4fb0d4b, 10, 7, ZenWaitKind::MWaitX, {0xb9, 0x02, 0x00, 0x00, 0x00, 0x31, 0xc0, 0x0f, 0x01, 0xfb}},
};

std::atomic<ZenWaitEntryFn> g_zen_wait_provider{nullptr};

const uint8_t kNop[6][5] = {{}, {0x90}, {0x66, 0x90}, {0x0f, 0x1f, 0x00}, {0x0f, 0x1f, 0x40, 0x00},
                            {0x0f, 0x1f, 0x44, 0x00, 0x00}};
}  // namespace

void set_zen_wait_provider(ZenWaitEntryFn fn) {
    g_zen_wait_provider.store(fn);
}

// 1: stub in place, 0: no host entry, so the instruction became a NOP (the loop then spins), -1: bytes differ or error.
int patch_zen_wait_window(uint64_t at, const uint8_t* expect, size_t len, size_t insn_off, ZenWaitKind kind) {
    if (len < 5 || insn_off + 3 != len || !readable(at, len) || memcmp(reinterpret_cast<const void*>(at), expect, len) != 0)
        return -1;
    const ZenWaitEntryFn provider = g_zen_wait_provider.load();
    const uint64_t entry = provider ? provider(kind) : 0;
    if (!entry) return code_write(at + insn_off, kNop[3], 3) == 0 ? 0 : -1;
    const auto probe = make_zen_wait_stub(0, expect, insn_off, kind, entry);
    const uint64_t stub = arena_reserve(at, probe.size(), nullptr);
    if (!stub || !arena_write(stub, make_zen_wait_stub(stub, expect, insn_off, kind, entry))) return -1;
    return write_call(at, len, stub) == GTA6RT_OK ? 1 : -1;
}

}  // namespace gta6rt::isa

extern "C" {

// CLZERO loops (ported from leonida runtime/src/clzero_patch.h, GPL-3.0-or-later): each of the two loop shapes
//   A (12 bytes)  clzero; add rax, 40h; cmp rax, END; jb A
//   B (15 bytes)  mov rax, CUR; add CUR, 40h; clzero; cmp CUR, END; jb B      (CUR, END: rdx or r8-r15)
// runs from a copy in the arena where CLZERO is eight qword stores of zero with rcx saved; the loop head becomes a
// jmp to it and the rest int3 (the jb targets the loop head only).
namespace {
struct ClzeroLoop {
    uint64_t head;
    uint32_t length;
    uint32_t clzero;
};
}  // namespace

static bool is_cmp_rr(const uint8_t* p) {
    return (p[0] == 0x48 || p[0] == 0x49 || p[0] == 0x4C || p[0] == 0x4D) && p[1] == 0x39 && (p[2] & 0xC0) == 0xC0;
}
static unsigned rm_reg(uint8_t rex, uint8_t modrm) { return (modrm & 7) | ((rex & 1) << 3); }
static unsigned reg_reg(uint8_t rex, uint8_t modrm) { return ((modrm >> 3) & 7) | ((rex & 4) << 1); }

static void find_clzero_loops(const uint8_t* code, size_t size, uint64_t addr, std::vector<ClzeroLoop>& out) {
    for (size_t k = 0; k + 3 <= size; ++k) {
        if (!(code[k] == 0x0F && code[k + 1] == 0x01 && code[k + 2] == 0xFC)) continue;
        if (k + 12 <= size) {
            const uint8_t* p = code + k;
            const bool add = p[3] == 0x48 && p[4] == 0x83 && p[5] == 0xC0 && p[6] == 0x40;
            const bool cmp = is_cmp_rr(p + 7) && rm_reg(p[7], p[9]) == 0 && (p[7] & 1) == 0;
            const unsigned end = cmp ? reg_reg(p[7], p[9]) : 0;
            const bool jb = p[10] == 0x72 && static_cast<int8_t>(p[11]) == -12;
            if (add && cmp && jb && end != 0 && end != 1 && end != 4) {
                out.push_back({addr + k, 12, 0});
                k += 2;
                continue;
            }
        }
        if (k >= 7 && k + 8 <= size) {
            const uint8_t* p = code + k - 7;
            const bool mov = (p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x89 && (p[2] & 0xC7) == 0xC0;
            const unsigned cur = mov ? reg_reg(p[0], p[2]) : 0;
            const bool add = (p[3] == 0x48 || p[3] == 0x49) && p[4] == 0x83 && (p[5] & 0xF8) == 0xC0 &&
                             rm_reg(p[3], p[5]) == cur && p[6] == 0x40;
            const bool cmp = is_cmp_rr(p + 10) && rm_reg(p[10], p[12]) == cur;
            const unsigned end = cmp ? reg_reg(p[10], p[12]) : 0;
            const bool jb = p[13] == 0x72 && static_cast<int8_t>(p[14]) == -15;
            const auto ok = [](unsigned r) { return r == 2 || r >= 8; };
            if (mov && add && cmp && jb && ok(cur) && ok(end) && cur != end) {
                out.push_back({addr + k - 7, 15, 7});
                k += 7;
            }
        }
    }
}

static std::vector<uint8_t> clzero_stub(const uint8_t* loop, const ClzeroLoop& s, uint64_t stub_at) {
    std::vector<uint8_t> out(loop, loop + s.clzero);
    static const uint8_t zero_line[] = {
        0x51, 0x48, 0x89, 0xC1, 0x48, 0x83, 0xE1, 0xC0, 0x48, 0xC7, 0x01, 0, 0, 0, 0,
        0x48, 0xC7, 0x41, 0x08, 0, 0, 0, 0, 0x48, 0xC7, 0x41, 0x10, 0, 0, 0, 0,
        0x48, 0xC7, 0x41, 0x18, 0, 0, 0, 0, 0x48, 0xC7, 0x41, 0x20, 0, 0, 0, 0,
        0x48, 0xC7, 0x41, 0x28, 0, 0, 0, 0, 0x48, 0xC7, 0x41, 0x30, 0, 0, 0, 0,
        0x48, 0xC7, 0x41, 0x38, 0, 0, 0, 0, 0x59,
    };
    out.insert(out.end(), std::begin(zero_line), std::end(zero_line));
    out.insert(out.end(), loop + s.clzero + 3, loop + s.length - 2);
    out.push_back(0x72);
    out.push_back(static_cast<uint8_t>(static_cast<int8_t>(-static_cast<int>(out.size() + 1))));
    out.push_back(0xE9);
    const int64_t rel = static_cast<int64_t>(s.head + s.length) - static_cast<int64_t>(stub_at + out.size() + 4);
    const int32_t rel32 = static_cast<int32_t>(rel);
    const auto* b = reinterpret_cast<const uint8_t*>(&rel32);
    out.insert(out.end(), b, b + 4);
    return out;
}

static gta6rt_status patch_clzero_loops(gta6rt_gva base, size_t size, uint32_t* patched) {
    if (!readable(base, 0x400)) return GTA6RT_E_INVALID_ARG;
    const auto* img = reinterpret_cast<const uint8_t*>(base);
    uint32_t lfanew = 0;
    memcpy(&lfanew, img + 0x3c, 4);
    if (lfanew > 0x300 || memcmp(img + lfanew, "PE\0\0", 4) != 0) return GTA6RT_E_BAD_FORMAT;
    uint16_t nsec = 0, optsz = 0;
    memcpy(&nsec, img + lfanew + 6, 2);
    memcpy(&optsz, img + lfanew + 20, 2);
    const uint8_t* sec = img + lfanew + 24 + optsz;
    std::vector<ClzeroLoop> loops;
    for (uint16_t i = 0; i < nsec; ++i, sec += 40) {
        uint32_t vsize = 0, rva = 0, raw = 0, ch = 0;
        memcpy(&vsize, sec + 8, 4);
        memcpy(&rva, sec + 12, 4);
        memcpy(&raw, sec + 16, 4);
        memcpy(&ch, sec + 36, 4);
        if (!(ch & 0x20000000u) || rva >= size) continue;
        const size_t n = std::min<size_t>(std::min(vsize, raw), size - rva);
        find_clzero_loops(img + rva, n, base + rva, loops);
    }
    for (const ClzeroLoop& s : loops) {
        const auto* loop = reinterpret_cast<const uint8_t*>(s.head);
        const auto probe = clzero_stub(loop, s, 0);
        const uint64_t at = arena_reserve(s.head, probe.size(), nullptr);
        if (!at) return GTA6RT_E_NO_MEMORY;
        if (!arena_write(at, clzero_stub(loop, s, at))) return GTA6RT_E_ACCESS;
        const int64_t rel = static_cast<int64_t>(at) - static_cast<int64_t>(s.head + 5);
        if (rel != static_cast<int32_t>(rel)) return GTA6RT_E_UNSUPPORTED;
        uint8_t b[15];
        b[0] = 0xE9;
        const int32_t r32 = static_cast<int32_t>(rel);
        memcpy(b + 1, &r32, 4);
        memset(b + 5, 0xCC, s.length - 5);
        if (code_write(s.head, b, s.length) != 0) return GTA6RT_E_ACCESS;
        ++*patched;
    }
    return GTA6RT_OK;
}

static gta6rt_status apply_zen_fixups(gta6rt_gva base, size_t size, gta6rt_isa_fixup_report* rep) {
    uint32_t stubs = 0, nops = 0;
    for (const ZenWaitSite& w : kZenWaitSites) {
        const int r = w.rva + w.len <= size ? patch_zen_wait_window(base + w.rva, w.bytes, w.len, w.insn_off, w.kind) : -1;
        if (r < 0) {
            rep->mismatched++;
            continue;
        }
        if (r) {
            stubs++;
            rep->zen_wait++;
        } else {
            nops++;
            rep->zen_nop++;
        }
    }
    if (nops) log_line("zen fixups: %u MONITORX/MWAITX sites became NOPs (no host wait entry): that wait loop spins", nops);
    else if (stubs) log_line("zen fixups: %u MONITORX/MWAITX sites call the host wait emulation", stubs);
    for (const ZenSite& z : kZenNopSites) {
        const uint64_t a = base + z.rva;
        if (z.rva + z.len > size || !readable(a, z.len) || memcmp(reinterpret_cast<const void*>(a), z.bytes, z.len) != 0) {
            rep->mismatched++;
            continue;
        }
        if (code_write(a, kNop[z.len], z.len) != 0) return GTA6RT_E_ACCESS;
        rep->zen_nop++;
    }
    return patch_clzero_loops(base, size, &rep->clzero_loops);
}

gta6rt_status gta6rt_isa_apply_image_fixups(gta6rt_gva image_base, size_t image_size, uint32_t flags,
                                             gta6rt_isa_fixup_report* out) {
    gta6rt_isa_fixup_report rep{};
    bool chkstk_done = false;
    for (const TitleSite& s : kTitleSites) {
        const uint64_t a = image_base + s.rva;
        const size_t n = s.kind == kSiteChkstk ? sizeof kChkstk : s.len;
        if (s.rva + n > image_size || !readable(a, n)) {
            rep.mismatched++;
            continue;
        }
        const auto* p = reinterpret_cast<const uint8_t*>(a);
        const bool match = s.kind == kSiteChkstk ? chkstk_matches(p) : memcmp(p, s.bytes, s.len) == 0;
        if (!match) {
            rep.mismatched++;
            continue;
        }
        gta6rt_status st = GTA6RT_OK;
        switch (s.kind) {
        case kSiteChkstk:
            if (flags & GTA6RT_ISA_FIXUP_CHKSTK) {
                st = gta6rt_isa_patch_chkstk(a);
                if (st == GTA6RT_OK) {
                    rep.chkstk++;
                    chkstk_done = true;
                }
            }
            break;
        case kSiteSse4a:
            if (flags & GTA6RT_ISA_FIXUP_SSE4A_PATCH) {
                st = gta6rt_isa_patch_sse4a_site(a);
                if (st == GTA6RT_OK) rep.sse4a++;
            }
            break;
        case kSiteVshiftPatch:
            if (vshift_check_enabled()) {
                st = patch_vshift_check_site(a, s.rva, false, nullptr);
                if (st == GTA6RT_OK) rep.vshift_checked++;
            } else if (flags & GTA6RT_ISA_FIXUP_VSHIFT) {
                st = gta6rt_isa_patch_vshift_site(a);
                if (st == GTA6RT_OK) rep.vshift++;
            }
            break;
        case kSiteVshiftSafe:
            rep.vshift_safe++;
            if (vshift_check_enabled()) {
                st = patch_vshift_check_site(a, s.rva, true, nullptr);
                if (st == GTA6RT_OK) rep.vshift_checked++;
            }
            break;
        case kSiteCpuid:
        case kSiteXgetbv:
            if (flags & GTA6RT_ISA_FIXUP_CPUID) {
                st = gta6rt_isa_patch_cpuid_site(a);
                if (st == GTA6RT_OK) (s.kind == kSiteCpuid ? rep.cpuid : rep.xgetbv)++;
            }
            break;
        }
        if (st != GTA6RT_OK) {
            log_line("fixup at rva 0x%x (kind %u) failed: %d", s.rva, s.kind, st);
            if (out) *out = rep;
            return st;
        }
    }
    rep.known_image = rep.mismatched == 0;
    if (rep.known_image && (flags & GTA6RT_ISA_FIXUP_CPUID)) g_cpuid_trapped = true;
    if (flags & GTA6RT_ISA_FIXUP_ZEN) {
        gta6rt_isa_fixup_report zen{};
        const gta6rt_status st = rep.known_image ? apply_zen_fixups(image_base, image_size, &zen)
                                                 : patch_clzero_loops(image_base, image_size, &zen.clzero_loops);
        rep.zen_nop = zen.zen_nop;
        rep.zen_wait = zen.zen_wait;
        rep.clzero_loops = zen.clzero_loops;
        rep.mismatched += zen.mismatched;
        if (st != GTA6RT_OK) {
            log_line("zen fixups failed: %d", st);
            if (out) *out = rep;
            return st;
        }
    }
    if ((flags & GTA6RT_ISA_FIXUP_CHKSTK) && !chkstk_done) {
        const gta6rt_gva f = gta6rt_isa_find_chkstk(image_base, image_size);
        if (f && gta6rt_isa_patch_chkstk(f) == GTA6RT_OK) rep.chkstk++;
    }
    if (out) *out = rep;
    return GTA6RT_OK;
}

}  // extern "C"
