#!/usr/bin/env python3
"""Executed-unwind test vectors for seh_pdata_exec_test, from a PE image's own .pdata.

usage: gen_vectors.py <image> <out.txt> [stride]

Needs pefile and capstone (spike/s1-seh/venv has both). Each vector is a chain of segments:

    V <kind> <n> s1 t1 ... sn tn      (hex RVAs)

The harness enters the function at s1 with known nonvolatile registers, and every time it reaches the int3 placed
at t_i (i < n) it continues at s_(i+1); reaching t_n is the trap point that gets unwound. Kinds:
    P  trap at a prolog instruction boundary (1 segment)
    E  run the prolog, jump to the body's restore moves before an epilog, trap at an epilog boundary
    C  chained fragment: run the root prolog and each ancestor fragment's prolog, trap in this fragment's prolog
    F  chained fragment: as C with the fragment's prolog complete, then trap at an epilog boundary in the fragment
A header line "K <rva>" names __chkstk, which the harness patches to ret so that probing prologs can run.

Prologs that branch are not run, and neither are prologs that access memory other than through rsp, rip, an
argument register or a register the prolog set from rsp (mov rax, rsp / lea rbp, [rsp+x]), without index (shrink-wrapped and scheduled prologs interleave body loads through registers
that the harness does not set up). For E/F vectors the harness itself puts the moved-saved nonvolatile registers
back before jumping to the epilog start (the body may interleave their restores with code it cannot run), and
sets pushed registers to junk so that the epilog's pops must be emulated by the unwinder. Changes from spike/s1-seh/probe/gen_vectors.py: restore moves are only [rsp + imm] loads (the spike also took
indexed loads such as mov rax, [rsp+rax*8+disp] and ran them with an uninitialised index: the 12 harness SIGSEGVs
in REVIEW.md); every canonical epilog of a function is used, not only the first, including pops + jmp tail calls;
__chkstk prologs and chained fragments are covered.
"""
import bisect
import collections
import re
import struct
import sys

import capstone
import pefile

REG = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"]
BRANCHY = ("call", "ret", "int3", "jmp", "hlt", "syscall", "int", "ud2", "loop")
RESTORE_GPR = re.compile(r"^\w+, qword ptr \[rsp( \+ (0x[0-9a-f]+|\d+))?\]$")
RESTORE_XMM = re.compile(r"^xmm\d+, xmmword ptr \[rsp( \+ (0x[0-9a-f]+|\d+))?\]$")
CHKSTK_BYTES = bytes.fromhex("4883ec104c8914244c895c2408")
MEM = re.compile(r"ptr \[(\w+)( [+-] (0x[0-9a-f]+|\d+))?\]")
LEA_STACK = re.compile(r"^(\w+), \[(\w+)( [+-] (0x[0-9a-f]+|\d+))?\]$")


class Image:
    def __init__(self, path):
        pe = pefile.PE(path, fast_load=True)
        oh = pe.OPTIONAL_HEADER
        self.base = oh.ImageBase
        self.mem = bytearray(oh.SizeOfImage)
        raw = pe.__data__
        for s in pe.sections:
            sz = min(s.SizeOfRawData, s.Misc_VirtualSize)
            self.mem[s.VirtualAddress:s.VirtualAddress + sz] = raw[s.PointerToRawData:s.PointerToRawData + sz]
        d = oh.DATA_DIRECTORY[3]
        self.pdata = (d.VirtualAddress, d.Size)

    def u8(self, a): return self.mem[a]
    def u16(self, a): return struct.unpack_from("<H", self.mem, a)[0]
    def u32(self, a): return struct.unpack_from("<I", self.mem, a)[0]


def opcode_slots(op, info, version):
    if op == 1: return 2 if info == 0 else 3
    if op in (4, 8): return 2
    if op in (5, 9): return 3
    if op == 6: return 1 if version == 2 else 2
    if op == 7: return 3
    return 1


def parse_unwind(img, rva):
    b0 = img.u8(rva)
    ver, flags = b0 & 7, b0 >> 3
    prolog, cnt, fr = img.u8(rva + 1), img.u8(rva + 2), img.u8(rva + 3)
    ops, i = [], 0
    while i < cnt:
        off, b1 = img.u8(rva + 4 + 2 * i), img.u8(rva + 5 + 2 * i)
        op, info = b1 & 15, b1 >> 4
        arg = None
        if op == 1:
            arg = img.u16(rva + 6 + 2 * i) * 8 if info == 0 else img.u32(rva + 6 + 2 * i)
        elif op == 2:
            arg = info * 8 + 8
        elif op in (4, 8):
            arg = img.u16(rva + 6 + 2 * i) * (8 if op == 4 else 16)
        elif op in (5, 9):
            arg = img.u32(rva + 6 + 2 * i)
        ops.append((off, op, info, arg))
        i += opcode_slots(op, info, ver)
    tail = rva + 4 + 2 * ((cnt + 1) & ~1)
    u = dict(ver=ver, flags=flags, prolog=prolog, cnt=cnt, freg=fr & 15, foff=(fr >> 4) * 16, ops=ops)
    if flags & 4:
        u["chain"] = (img.u32(tail), img.u32(tail + 4), img.u32(tail + 8))
    return u


class Gen:
    def __init__(self, img):
        self.img = img
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        prva, psz = img.pdata
        self.funcs = [struct.unpack_from("<III", img.mem, prva + 12 * i) for i in range(psz // 12)]
        self.by_begin = {f[0]: f for f in self.funcs}
        self.begins = [f[0] for f in self.funcs]
        self.chkstk = self.find_chkstk()
        self.stats = collections.Counter()

    def unwind(self, u):
        while u & 1:
            u = self.img.u32((u & ~1) + 8)
        return parse_unwind(self.img, u)

    def find_chkstk(self):
        c = collections.Counter()
        for b, e, u in self.funcs:
            ui = self.unwind(u)
            if not ui["prolog"]:
                continue
            for ins in self.md.disasm(bytes(self.img.mem[b:b + ui["prolog"]]), b):
                if ins.mnemonic == "call" and ins.op_str.startswith("0x"):
                    c[int(ins.op_str, 16)] += 1
        for t, _ in c.most_common(3):
            f = t
            if self.img.mem[f] == 0xe9:
                f = f + 5 + struct.unpack_from("<i", self.img.mem, f + 1)[0]
            if bytes(self.img.mem[f:f + len(CHKSTK_BYTES)]) == CHKSTK_BYTES:
                return t
        return None

    def prolog_insns(self, b, size):
        """Prolog instructions, or None if they cannot be run by the harness."""
        if not size:
            return []
        insns = list(self.md.disasm(bytes(self.img.mem[b:b + size]), b))
        if sum(i.size for i in insns) != size:
            return None
        safe = {"rsp", "rip", "rcx", "rdx", "r8", "r9"}
        for i in insns:
            if i.mnemonic == "call" and self.chkstk is not None and i.op_str == hex(self.chkstk):
                continue
            if i.mnemonic.startswith(BRANCHY) or i.mnemonic.startswith("j"):
                return None
            if i.mnemonic != "lea" and "ptr [" in i.op_str:
                m = MEM.search(i.op_str)
                if not m or m.group(1) not in safe:
                    return None
            dst = i.op_str.split(",")[0].strip()
            if i.mnemonic == "mov" and re.match(r"^\w+, rsp$", i.op_str):
                safe.add(dst)
            elif i.mnemonic == "lea" and LEA_STACK.match(i.op_str) and LEA_STACK.match(i.op_str).group(2) in safe:
                safe.add(dst)
            elif dst in safe and dst != "rsp" and "ptr [" not in dst:
                safe.discard(dst)
        return insns

    def epilogs(self, lo, hi, prolog_end, pushes, alloc, freg):
        """(restore start, [boundaries]) for each canonical epilog in [prolog_end, hi)."""
        body = list(self.md.disasm(bytes(self.img.mem[prolog_end:hi]), prolog_end))
        out = []
        for k, ins in enumerate(body):
            seq, j = [], k
            if alloc:
                if freg and ins.mnemonic == "lea" and ins.op_str.startswith("rsp, [" + REG[freg]):
                    seq.append(ins)
                elif not freg and ins.mnemonic == "add" and ins.op_str == "rsp, %#x" % alloc:
                    seq.append(ins)
                else:
                    continue
                j += 1
            elif k and body[k - 1].mnemonic == "pop":
                continue
            pops = []
            while j < len(body) and body[j].mnemonic == "pop":
                pops.append(body[j].op_str)
                seq.append(body[j])
                j += 1
            if j >= len(body) or pops != list(reversed(pushes)):
                continue
            t = body[j]
            if t.mnemonic == "ret":
                pass
            elif t.mnemonic == "jmp":
                if t.op_str.startswith("0x"):
                    tgt = int(t.op_str, 16)
                    if lo <= tgt < hi:
                        continue
                    te, se = self.entry_at(tgt), self.entry_at(lo)
                    if te and se and self.root_of(te) == self.root_of(se):
                        continue
                elif not t.op_str.startswith("qword ptr [rip"):
                    continue
            else:
                continue
            seq.append(t)
            start = k
            while start > 0 and (
                    (body[start - 1].mnemonic == "mov" and RESTORE_GPR.match(body[start - 1].op_str)) or
                    (body[start - 1].mnemonic in ("movaps", "vmovaps", "movapd", "vmovapd", "movdqa", "movups",
                                                  "vmovups", "movdqu", "vmovdqu") and
                     RESTORE_XMM.match(body[start - 1].op_str))):
                start -= 1
            out.append((body[start].address, [s.address for s in seq]))
        return out

    def entry_at(self, rva):
        i = bisect.bisect_right(self.begins, rva) - 1
        if i >= 0 and self.funcs[i][0] <= rva < self.funcs[i][1]:
            return self.funcs[i]
        return None

    def root_of(self, f):
        c = self.chain_of(f)
        return c[0] if c else None

    def chain_of(self, f):
        chain = [f]
        while True:
            ui = self.unwind(chain[-1][2])
            if not ui["flags"] & 4:
                return list(reversed(chain))
            pb = ui["chain"][0]
            if pb not in self.by_begin or len(chain) > 32:
                return None
            chain.append(self.by_begin[pb])

    def gen(self, out, stride):
        for idx, f in enumerate(self.funcs):
            if idx % stride:
                continue
            b, e, u = f
            ui = self.unwind(u)
            if ui["flags"] & 4:
                self.gen_chained(out, f)
                continue
            pro = self.prolog_insns(b, ui["prolog"])
            if pro is None:
                self.stats["skipped: branch or memory access through body registers in prolog"] += 1
                continue
            if any(i.mnemonic == "call" for i in pro):
                self.stats["chkstk prologs"] += 1
            for off in [0] + [i.address + i.size - b for i in pro]:
                out.write("V P 1 %x %x\n" % (b, b + off))
                self.stats["P"] += 1
            pushes = [REG[info] for (_, op, info, _) in reversed(ui["ops"]) if op == 0]
            alloc = sum(arg for (_, op, _, arg) in ui["ops"] if op in (1, 2))
            for start, bounds in self.epilogs(b, e, b + ui["prolog"], pushes, alloc, ui["freg"]):
                for t in bounds:
                    out.write("V E 2 %x %x %x %x\n" % (b, b + ui["prolog"], start, t))
                    self.stats["E"] += 1

    def gen_chained(self, out, f):
        chain = self.chain_of(f)
        if chain is None:
            self.stats["skipped: broken chain"] += 1
            return
        segs = []
        pushes, alloc, freg = [], 0, 0
        for lvl, (b, e, u) in enumerate(chain):
            ui = self.unwind(u)
            pro = self.prolog_insns(b, ui["prolog"])
            if pro is None:
                self.stats["skipped: chained, branch or memory access through body registers in prolog"] += 1
                return
            if lvl and any(off == 0 or off > ui["prolog"] for (off, _, _, _) in ui["ops"]):
                self.stats["skipped: chained, codes not performed by the fragment's own prolog"] += 1
                return
            if any(op not in (0, 1, 2, 3, 4, 5, 8, 9) for (_, op, _, _) in ui["ops"]):
                self.stats["skipped: chained, unusual codes"] += 1
                return
            pushes = [REG[info] for (_, op, info, _) in reversed(ui["ops"]) if op == 0] + pushes
            alloc += sum(arg for (_, op, _, arg) in ui["ops"] if op in (1, 2))
            freg = freg or ui["freg"]
            if lvl < len(chain) - 1:
                segs.append((b, b + ui["prolog"]))
            else:
                bounds = [0] + [i.address + i.size - b for i in pro]
        b, e, u = chain[-1]
        ui = self.unwind(u)
        for off in bounds:
            s = segs + [(b, b + off)]
            out.write("V C %d %s\n" % (len(s), " ".join("%x %x" % p for p in s)))
            self.stats["C"] += 1
        full = segs + [(b, b + ui["prolog"])]
        for start, eb in self.epilogs(b, e, b + ui["prolog"], pushes, alloc, freg):
            for t in eb:
                s = full + [(start, t)]
                out.write("V F %d %s\n" % (len(s), " ".join("%x %x" % p for p in s)))
                self.stats["F"] += 1


def main():
    img = Image(sys.argv[1])
    stride = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    g = Gen(img)
    with open(sys.argv[2], "w") as out:
        if g.chkstk is not None:
            out.write("K %x\n" % g.chkstk)
        g.gen(out, stride)
    print("image %s base %#x functions %d stride %d chkstk %s" % (sys.argv[1], img.base, len(g.funcs), stride,
                                                                   hex(g.chkstk) if g.chkstk is not None else "none"))
    for k in sorted(g.stats):
        print("  %s: %d" % (k, g.stats[k]))


if __name__ == "__main__":
    main()
