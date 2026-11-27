#!/usr/bin/env python3
"""Count-operand analysis for every vpsrlvq/vpsllvq site in the exe (Rosetta bug, NOTES S0.6).

Rosetta mis-executes a lane iff its 64-bit count c has (c >> 32) != 0 and (c & 0xffffffff) < 64
(spike/s0.2-avx/out/shift_probe.txt). A site is SAFE if every count lane provably has a zero high dword or a
low dword >= 64 on every path; otherwise PATCH.

Abstract domain: per dword of a 256-bit register, an unsigned interval [lo, hi] in 0..2^32-1. Values come from
reaching definitions over the CFG of the enclosing .pdata range (indirect-jump targets and the function entry
are unknown). Any cycle in the definition chain (loop-carried values) gives TOP. Unknown instructions give TOP.

usage: vshift_analyze.py EXE SITES.json OUT.json [--trace RVA]
"""
import sys, json, struct
import pefile, capstone
from capstone import x86_const as X

M32 = 0xffffffff
# [rsp+N] slots are matched by exact displacement only (no rsp adjustment or aliasing model), so they are
# TOP unless --stack is given; the verdicts must not depend on them.
STACK_TRACKING = '--stack' in sys.argv
TOPD = (0, M32)
TOP = [TOPD] * 8
ZERO = [(0, 0)] * 8


def const_dw(lst):
    return [(v, v) for v in lst]


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.secs = [(s.VirtualAddress, max(s.Misc_VirtualSize, s.SizeOfRawData), s.get_data()) for s in self.pe.sections]
        t = next(s for s in self.pe.sections if s.Name.rstrip(b'\0') == b'.text')
        self.text_rva, self.text = t.VirtualAddress, t.get_data()
        self.writable = [(s.VirtualAddress, s.Misc_VirtualSize) for s in self.pe.sections if s.Characteristics & 0x80000000]

    def read(self, rva, n):
        for va, sz, data in self.secs:
            if va <= rva and rva + n <= va + sz:
                b = data[rva - va:rva - va + n]
                return b + b'\0' * (n - len(b))
        return None

    def relocs_near(self, rva, n):
        if not hasattr(self, '_relocs'):
            import bisect
            self.pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_BASERELOC']])
            rs = []
            for blk in getattr(self.pe, 'DIRECTORY_ENTRY_BASERELOC', []):
                for e in blk.entries:
                    if e.type:
                        rs.append(e.rva)
            self._relocs = sorted(rs)
            self._bis = bisect
        k = self._bis.bisect_left(self._relocs, rva - 8)
        out = []
        while k < len(self._relocs) and self._relocs[k] < rva + n:
            out.append(self._relocs[k])
            k += 1
        return out

    def unwind_flags(self, begin):
        pd = next(s for s in self.pe.sections if s.Name.rstrip(b'\0') == b'.pdata').get_data()
        if not hasattr(self, '_pd'):
            self._pd = {}
            for k in range(0, len(pd) - 11, 12):
                b, e, u = struct.unpack_from('<III', pd, k)
                self._pd[b] = u
        u = self._pd.get(begin)
        if u is None:
            return -1
        b = self.read(u & ~1, 1)
        return b[0] >> 3 if b else -1

    def is_writable(self, rva):
        return any(va <= rva < va + sz for va, sz in self.writable)


GPR = {}
for full, names in {
    'rax': 'rax eax ax al ah', 'rbx': 'rbx ebx bx bl bh', 'rcx': 'rcx ecx cx cl ch', 'rdx': 'rdx edx dx dl dh',
    'rsi': 'rsi esi si sil', 'rdi': 'rdi edi di dil', 'rbp': 'rbp ebp bp bpl', 'rsp': 'rsp esp sp spl'}.items():
    for n in names.split():
        GPR[n] = full
for i in range(8, 16):
    for suf in ('', 'd', 'w', 'b'):
        GPR['r%d%s' % (i, suf)] = 'r%d' % i


def vreg(name):
    if name and name[:3] in ('xmm', 'ymm'):
        return 'v' + name[3:], (128 if name[0] == 'x' else 256)
    return None, 0


def regkey(md, r):
    n = md.reg_name(r)
    v, _ = vreg(n)
    if v:
        return v
    return GPR.get(n, n)


class Func:
    def __init__(self, img, md, b, e):
        self.img, self.md = img, md
        code = img.text[b - img.text_rva:e - img.text_rva]
        self.insns = list(md.disasm(code, b))
        self.idx = {i.address: k for k, i in enumerate(self.insns)}
        leaders = {self.insns[0].address}
        self.succ = {}
        for k, i in enumerate(self.insns):
            nxt = self.insns[k + 1].address if k + 1 < len(self.insns) else None
            g = set(i.groups)
            tgt = None
            if X.X86_GRP_JUMP in g or X.X86_GRP_RET in g:
                if nxt:
                    leaders.add(nxt)
                if i.operands and i.operands[0].type == X.X86_OP_IMM:
                    tgt = i.operands[0].imm
                    if tgt in self.idx:
                        leaders.add(tgt)
        self.preds = {i.address: [] for i in self.insns}
        self.indirect_targets_unknown = False
        for k, i in enumerate(self.insns):
            nxt = self.insns[k + 1].address if k + 1 < len(self.insns) else None
            g = set(i.groups)
            if X.X86_GRP_RET in g:
                continue
            if X.X86_GRP_JUMP in g:
                if i.operands and i.operands[0].type == X.X86_OP_IMM:
                    t = i.operands[0].imm
                    if t in self.idx:
                        self.preds[t].append(i.address)
                else:
                    self.indirect_targets_unknown = True
                if i.mnemonic != 'jmp' and nxt:
                    self.preds[nxt].append(i.address)
                continue
            if i.mnemonic in ('int3', 'ud2'):
                continue
            if nxt:
                self.preds[nxt].append(i.address)
        self.entry = self.insns[0].address
        self.ext = set()
        self.indirect = [i.address for i in self.insns if X.X86_GRP_JUMP in set(i.groups) and not (
            i.operands and i.operands[0].type == X.X86_OP_IMM)]


class Analyzer:
    def __init__(self, img):
        self.img = img
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.md.detail = True
        self.notes = []

    # ---- reaching definitions -------------------------------------------------
    def writes(self, f, i, key):
        if i.mnemonic in ('call',):
            # Win64: rax rcx rdx r8-r11, xmm0-5 and the upper halves of every ymm are volatile.
            return key in ('rax', 'rcx', 'rdx', 'r8', 'r9', 'r10', 'r11') or key.startswith('v')
        if i.mnemonic in ('vzeroupper', 'vzeroall'):
            return key.startswith('v')
        if key.startswith('stk:'):
            if i.operands and i.operands[0].type == X.X86_OP_MEM and (i.operands[0].access & capstone.CS_AC_WRITE):
                m = i.operands[0].mem
                if self.md.reg_name(m.base) == 'rsp' and m.index == 0:
                    return 'stk:%d' % m.disp == key
            return False
        try:
            _, w = i.regs_access()
        except capstone.CsError:
            return True
        return any(regkey(self.md, r) == key for r in w)

    def reaching(self, f, addr, key):
        """All instructions that may define key and reach addr (before it). 'ENTRY' if unknown inflow."""
        out, seen = set(), set()
        stack = [(addr, False)]
        while stack:
            a, include = stack.pop()
            k = f.idx[a]
            start = k if include else k - 1
            found = False
            j = start
            while j >= 0:
                ins = f.insns[j]
                if self.writes(f, ins, key):
                    out.add(ins.address)
                    found = True
                    break
                if j < start or include:
                    pass
                pr = f.preds[ins.address]
                is_leader = (j == 0) or (pr != [f.insns[j - 1].address]) or ins.address in f.ext
                if is_leader:
                    # An instruction with no predecessor is dead padding unless something we cannot see may
                    # jump to it: an indirect jump in this range or an exception handler continuation.
                    if ins.address == f.entry or ins.address in f.ext or (not pr and f.orphans_reachable):
                        out.add('ENTRY')
                    for p in pr:
                        if p not in seen:
                            seen.add(p)
                            stack.append((p, True))
                    found = True
                    break
                j -= 1
            if not found:
                out.add('ENTRY')
        return out

    # ---- values ----------------------------------------------------------------
    def val_vreg(self, f, at, key, width, depth, path):
        defs = self.reaching(f, at, key)
        res = None
        for d in defs:
            if d == 'ENTRY':
                v = list(TOP)
                self.notes.append('%s reaches from function entry/unknown inflow' % key)
            elif (d, key) in path:
                v = list(TOP)
                self.notes.append('%s: loop-carried definition at %x' % (key, d))
            else:
                v = self.eval_def(f, f.insns[f.idx[d]], key, depth + 1, path | {(d, key)})
            res = v if res is None else join(res, v)
        if width == 128:
            res = res[:4] + [(0, 0)] * 4
        return res

    def val_gpr(self, f, at, key, depth, path):
        defs = self.reaching(f, at, key)
        res = None
        for d in defs:
            if d == 'ENTRY' or (d, key) in path:
                v = (0, 2 ** 64 - 1)
            else:
                v = self.eval_gpr_def(f, f.insns[f.idx[d]], key, depth + 1, path | {(d, key)})
            res = v if res is None else (min(res[0], v[0]), max(res[1], v[1]))
        return res

    def mem_const(self, i, op, n):
        m = op.mem
        if self.md.reg_name(m.base) == 'rip' and m.index == 0:
            rva = i.address + i.size + m.disp
            if any(rva - 8 < r < rva + n for r in self.img.relocs_near(rva, n)):
                self.notes.append('relocation inside constant at %x' % rva)
                return None
            if self.img.is_writable(rva):
                self.notes.append('memory operand at %x is in a writable section' % rva)
                return None
            return self.img.read(rva, n)
        return None

    def opval(self, f, i, op, depth, path, nbytes=32):
        """Value of a vector operand (register or memory) read by i, as 8 dword intervals."""
        if op.type == X.X86_OP_REG:
            key, w = vreg(self.md.reg_name(op.reg))
            return self.val_vreg(f, i.address, key, w, depth, path)
        if op.type == X.X86_OP_MEM:
            b = self.mem_const(i, op, nbytes)
            if b is not None:
                dw = list(struct.unpack('<%dI' % (len(b) // 4), b)) + [0] * 8
                return const_dw(dw[:8])
            m = op.mem
            if STACK_TRACKING and self.md.reg_name(m.base) == 'rsp' and m.index == 0:
                return self.val_vreg(f, i.address, 'stk:%d' % m.disp, 256 if nbytes == 32 else 128, depth, path)
            return list(TOP)
        return list(TOP)

    def eval_def(self, f, i, key, depth, path):
        if depth > 40:
            return list(TOP)
        if key.startswith('stk:'):
            src = i.operands[1] if len(i.operands) > 1 else None
            if i.mnemonic in ('vmovdqa', 'vmovdqu', 'vmovaps', 'vmovups') and src is not None and src.type == X.X86_OP_REG:
                k, w = vreg(self.md.reg_name(src.reg))
                v = self.val_vreg(f, i.address, k, w, depth, path)
                return v
            return list(TOP)
        mn = i.mnemonic
        ops = i.operands
        if mn == 'call':
            n = int(key[1:])
            if n < 6:
                return list(TOP)
            lo = self.val_vreg(f, i.address, key, 128, depth, path)
            return lo[:4] + [TOPD] * 4
        if mn in ('vzeroupper', 'vzeroall'):
            if mn == 'vzeroall':
                return list(ZERO)
            lo = self.val_vreg(f, i.address, key, 128, depth, path)
            return lo[:4] + [(0, 0)] * 4
        dst = ops[0] if ops else None
        if dst is None or dst.type != X.X86_OP_REG or vreg(self.md.reg_name(dst.reg))[0] != key:
            return list(TOP)
        _, w = vreg(self.md.reg_name(dst.reg))
        vex = i.bytes[0] in (0xc4, 0xc5)
        r = self.sem(f, i, mn, ops, w, depth, path)
        if r is None:
            self.notes.append('unmodelled def %x %s %s' % (i.address, mn, i.op_str))
            r = list(TOP)
        if w == 128:
            if vex:
                r = r[:4] + [(0, 0)] * 4
            else:
                up = self.val_vreg(f, i.address, key, 256, depth, path)
                r = r[:4] + up[4:]
        return r

    def sem(self, f, i, mn, ops, w, depth, path):
        def V(k):
            return self.opval(f, i, ops[k], depth, path, 32 if w == 256 else 16)

        def imm(k):
            return ops[k].imm & 0xff

        if mn in ('vmovdqa', 'vmovdqu', 'vmovaps', 'vmovups', 'vmovapd', 'vmovupd'):
            return V(1)
        if mn in ('vpxor', 'vxorps', 'vxorpd') and ops[1].type == X.X86_OP_REG and ops[2].type == X.X86_OP_REG and ops[1].reg == ops[2].reg:
            return list(ZERO)
        if mn in ('vpand', 'vandps', 'vandpd'):
            a, b = V(1), V(2)
            return [(0, min(x[1], y[1])) for x, y in zip(a, b)]
        if mn in ('vpandn', 'vandnps'):
            b = V(2)
            return [(0, y[1]) for y in b]
        if mn in ('vpor', 'vorps', 'vpxor', 'vxorps'):
            a, b = V(1), V(2)
            out = []
            for x, y in zip(a, b):
                if x == (0, 0):
                    out.append(y)
                elif y == (0, 0):
                    out.append(x)
                else:
                    hb = max(x[1], y[1]).bit_length()
                    lo = max(x[0], y[0]) if mn in ('vpor', 'vorps') else 0
                    out.append((lo, (1 << hb) - 1))
            return out
        if mn == 'vpblendd':
            a, b, m = V(1), V(2), imm(3)
            return [b[k] if (m >> k) & 1 else a[k] for k in range(8)]
        if mn in ('vpsrlq', 'vpsllq', 'vpsrld', 'vpslld', 'vpsrad') and ops[2].type == X.X86_OP_IMM:
            a, s = V(1), imm(2)
            return shift_imm(mn, a, s)
        if mn in ('vpaddq', 'vpsubq'):
            a, b = V(1), V(2)
            out = []
            for q in range(4):
                x, y = q64(a, q), q64(b, q)
                if mn == 'vpaddq':
                    r = (x[0] + y[0], x[1] + y[1])
                else:
                    r = (x[0] - y[1], x[1] - y[0])
                out += split64(r)
            return out
        if mn in ('vpaddd', 'vpsubd'):
            a, b = V(1), V(2)
            out = []
            for x, y in zip(a, b):
                r = (x[0] + y[0], x[1] + y[1]) if mn == 'vpaddd' else (x[0] - y[1], x[1] - y[0])
                out.append(r if 0 <= r[0] and r[1] <= M32 else TOPD)
            return out
        if mn in ('vpmaxsd', 'vpminsd', 'vpmaxud', 'vpminud'):
            a, b = V(1), V(2)
            out = []
            for x, y in zip(a, b):
                if mn.endswith('sd'):
                    xs, ys = s32(x), s32(y)
                    if xs is None or ys is None:
                        if mn == 'vpmaxsd' and ys is not None and ys[0] >= 0:
                            out.append((ys[0], 0x7fffffff))
                        elif mn == 'vpmaxsd' and xs is not None and xs[0] >= 0:
                            out.append((xs[0], 0x7fffffff))
                        else:
                            out.append(TOPD)
                        continue
                    f2 = max if mn == 'vpmaxsd' else min
                    r = (f2(xs[0], ys[0]), f2(xs[1], ys[1]))
                    out.append(u32(r))
                else:
                    f2 = max if mn == 'vpmaxud' else min
                    out.append((f2(x[0], y[0]), f2(x[1], y[1])))
            return out
        if mn == 'vpshufb':
            a = V(1)
            ctl = self.const_bytes(f, i, ops[2], 32 if w == 256 else 16, depth, path)
            if ctl is None:
                return None
            out = []
            for d in range(8 if w == 256 else 4):
                cb = ctl[d * 4:d * 4 + 4]
                if all(c & 0x80 for c in cb):
                    out.append((0, 0))
                else:
                    nz = 0
                    for c in reversed(cb):
                        if c & 0x80:
                            nz += 1
                        else:
                            break
                    out.append((0, (1 << (8 * (4 - nz))) - 1))
            return out + [(0, 0)] * (8 - len(out))
        if mn == 'vpshufd':
            a, m = V(1), imm(2)
            out = []
            for half in range(2):
                for k in range(4):
                    out.append(a[half * 4 + ((m >> (2 * k)) & 3)])
            return out
        if mn in ('vpermq', 'vpermpd') and ops[2].type == X.X86_OP_IMM:
            a, m = V(1), imm(2)
            out = []
            for k in range(4):
                s = (m >> (2 * k)) & 3
                out += a[2 * s:2 * s + 2]
            return out
        if mn in ('vpbroadcastq', 'vbroadcastsd', 'vmovddup'):
            a = self.opval(f, i, ops[1], depth, path, 8)
            return a[:2] * 4
        if mn in ('vpbroadcastd', 'vbroadcastss'):
            a = self.opval(f, i, ops[1], depth, path, 4)
            return a[:1] * 8
        if mn in ('vpmovzxdq', 'vpmovzxbq', 'vpmovzxwq', 'vpmovsxdq', 'vpmovsxbq', 'vpmovsxwq'):
            sz = {'d': 4, 'w': 2, 'b': 1}[mn[7]]
            n = 4 if w == 256 else 2
            if ops[1].type == X.X86_OP_MEM:
                b = self.mem_const(i, ops[1], sz * n)
                if b is not None:
                    out = []
                    for k in range(n):
                        v = int.from_bytes(b[k * sz:(k + 1) * sz], 'little', signed=mn[5] == 's')
                        out += split64((v, v))
                    return out + [(0, 0)] * (8 - len(out))
            if mn[5] == 'z':
                lim = (1 << (8 * sz)) - 1
                return [(0, lim), (0, 0)] * 4
            src = self.opval(f, i, ops[1], depth, path, 16) if ops[1].type == X.X86_OP_REG else list(TOP)
            if sz == 4:
                out = []
                for k in range(n):
                    x = src[k]
                    out += [x, (0, 0)] if x[1] <= 0x7fffffff else [TOPD, TOPD]
                return out + [(0, 0)] * (8 - len(out))
            return list(TOP)
        if mn in ('vmovd',) and ops[1].type in (X.X86_OP_REG, X.X86_OP_MEM):
            if ops[1].type == X.X86_OP_REG and not vreg(self.md.reg_name(ops[1].reg))[0]:
                g = self.val_gpr(f, i.address, GPR[self.md.reg_name(ops[1].reg)], depth, path)
                return [clip32(g)] + [(0, 0)] * 7
            return [TOPD] + [(0, 0)] * 7
        if mn == 'vmovq' and ops[1].type == X.X86_OP_REG and not vreg(self.md.reg_name(ops[1].reg))[0]:
            g = self.val_gpr(f, i.address, GPR[self.md.reg_name(ops[1].reg)], depth, path)
            return split64(g) + [(0, 0)] * 6
        if mn == 'vmovq' and ops[1].type == X.X86_OP_MEM:
            b = self.mem_const(i, ops[1], 8)
            if b is not None:
                return const_dw(list(struct.unpack('<2I', b))) + [(0, 0)] * 6
            return [TOPD, TOPD] + [(0, 0)] * 6
        if mn == 'vmovq' and ops[1].type == X.X86_OP_REG:
            a = V(1)
            return a[:2] + [(0, 0)] * 6
        if mn == 'vpinsrq':
            a, k = V(1), imm(3) & 1
            if ops[2].type == X.X86_OP_REG:
                g = split64(self.val_gpr(f, i.address, GPR[self.md.reg_name(ops[2].reg)], depth, path))
            else:
                g = [TOPD, TOPD]
            a = list(a)
            a[2 * k:2 * k + 2] = g
            return a
        if mn in ('vpunpcklqdq', 'vpunpckhqdq'):
            a, b = V(1), V(2)
            o = 0 if mn == 'vpunpcklqdq' else 2
            out = []
            for half in range(2):
                h = half * 4
                out += a[h + o:h + o + 2] + b[h + o:h + o + 2]
            return out
        if mn in ('vpunpckldq', 'vpunpckhdq'):
            a, b = V(1), V(2)
            o = 0 if mn == 'vpunpckldq' else 2
            out = []
            for half in range(2):
                h = half * 4
                out += [a[h + o], b[h + o], a[h + o + 1], b[h + o + 1]]
            return out
        if mn == 'vinserti128' or mn == 'vinsertf128':
            a, b, m = V(1), self.opval(f, i, ops[2], depth, path, 16), imm(3) & 1
            a = list(a)
            a[4 * m:4 * m + 4] = b[:4]
            return a
        if mn in ('vextracti128', 'vextractf128'):
            a, m = self.opval(f, i, ops[1], depth, path, 32), imm(2) & 1
            return a[4 * m:4 * m + 4] + [(0, 0)] * 4
        if mn in ('vperm2i128', 'vperm2f128'):
            a, b, m = V(1), V(2), imm(3)
            src = a[:4], a[4:], b[:4], b[4:]
            lo = [(0, 0)] * 4 if m & 0x08 else src[m & 3]
            hi = [(0, 0)] * 4 if m & 0x80 else src[(m >> 4) & 3]
            return list(lo) + list(hi)
        if mn in ('vpcmpeqq', 'vpcmpgtq', 'vpcmpeqd', 'vpcmpgtd'):
            return list(TOP)
        return None

    def const_bytes(self, f, i, op, n, depth, path):
        if op.type == X.X86_OP_MEM:
            return self.mem_const(i, op, n)
        v = self.opval(f, i, op, depth, path, n)
        if all(x[0] == x[1] for x in v):
            return b''.join(struct.pack('<I', x[0]) for x in v)[:n]
        return None

    def eval_gpr_def(self, f, i, key, depth, path):
        mn, ops = i.mnemonic, i.operands
        full = (0, 2 ** 64 - 1)
        if not ops or ops[0].type != X.X86_OP_REG or GPR.get(self.md.reg_name(ops[0].reg)) != key:
            return full
        dn = self.md.reg_name(ops[0].reg)
        size = ops[0].size
        if size == 4:
            if mn == 'xor' and ops[1].type == X.X86_OP_REG and ops[1].reg == ops[0].reg:
                return (0, 0)
            if mn == 'mov' and ops[1].type == X.X86_OP_IMM:
                v = ops[1].imm & M32
                return (v, v)
            if mn == 'and' and ops[1].type == X.X86_OP_IMM:
                return (0, ops[1].imm & M32)
            if mn in ('movzx',):
                return (0, (1 << (8 * ops[1].size)) - 1)
            return (0, M32)
        if size == 8:
            if mn == 'mov' and ops[1].type == X.X86_OP_IMM:
                v = ops[1].imm & (2 ** 64 - 1)
                return (v, v)
            if mn == 'mov' and ops[1].type == X.X86_OP_REG:
                return self.val_gpr(f, i.address, GPR[self.md.reg_name(ops[1].reg)], depth, path)
            if mn == 'and' and ops[1].type == X.X86_OP_IMM and ops[1].imm >= 0:
                return (0, ops[1].imm)
            if mn in ('movzx',):
                return (0, (1 << (8 * ops[1].size)) - 1)
            if mn == 'xor' and ops[1].type == X.X86_OP_REG and ops[1].reg == ops[0].reg:
                return (0, 0)
        return full


def s32(x):
    if x[1] <= 0x7fffffff:
        return x
    if x[0] >= 0x80000000:
        return (x[0] - 2 ** 32, x[1] - 2 ** 32)
    return None


def u32(r):
    if r[0] >= 0:
        return r
    if r[1] < 0:
        return (r[0] + 2 ** 32, r[1] + 2 ** 32)
    return TOPD


def clip32(g):
    if 0 <= g[0] and g[1] <= M32:
        return g
    return TOPD


def q64(a, q):
    lo, hi = a[2 * q], a[2 * q + 1]
    if hi[0] == hi[1]:
        return (hi[0] * 2 ** 32 + lo[0], hi[0] * 2 ** 32 + lo[1])
    return (hi[0] * 2 ** 32, hi[1] * 2 ** 32 + M32)


def split64(r):
    lo, hi = r
    if lo < 0 and hi < 0 and lo >= -(2 ** 32):
        return [(lo + 2 ** 32, hi + 2 ** 32), (M32, M32)]
    if lo < 0 or hi > 2 ** 64 - 1:
        return [TOPD, TOPD]
    if hi >> 32 == lo >> 32:
        h = hi >> 32
        return [(lo & M32, hi & M32), (h, h)]
    return [TOPD, (lo >> 32, hi >> 32)]


def shift_imm(mn, a, s):
    out = []
    if mn in ('vpsrld', 'vpslld', 'vpsrad'):
        for x in a:
            if mn == 'vpsrld':
                out.append((0, 0) if s > 31 else (x[0] >> s, x[1] >> s))
            elif mn == 'vpslld':
                out.append((0, 0) if s > 31 else ((x[0] << s, x[1] << s) if (x[1] << s) <= M32 else TOPD))
            else:
                out.append((x[0] >> s, x[1] >> s) if x[1] <= 0x7fffffff and s <= 31 else TOPD)
        return out
    for q in range(4):
        x = q64(a, q)
        if s > 63:
            r = (0, 0)
        elif mn == 'vpsrlq':
            r = (x[0] >> s, x[1] >> s)
        else:
            r = (x[0] << s, x[1] << s)
            if r[1] > 2 ** 64 - 1:
                if s >= 32:
                    lo = a[2 * q]
                    hi = (lo[0] << (s - 32), lo[1] << (s - 32))
                    out += [(0, 0), hi if hi[1] <= M32 else TOPD]
                    continue
                r = (0, 2 ** 64 - 1)
        out += split64(r)
    return out


def join(a, b):
    return [(min(x[0], y[0]), max(x[1], y[1])) for x, y in zip(a, b)]


def lane_safe(lo, hi):
    return hi == (0, 0) or lo[0] >= 64


def main():
    exe, sitesp, outp = sys.argv[1:4]
    trace = int(sys.argv[5], 16) if len(sys.argv) > 5 and sys.argv[4] == '--trace' else None
    img = Image(exe)
    an = Analyzer(img)
    sites = [s for s in json.load(open(sitesp))['sites'] if s['mn'] in ('vpsrlvq', 'vpsllvq')]
    funcs = {}
    res = []
    ranges = sorted({tuple(s['range']) for s in sites})
    md2 = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    ext = {}
    code = img.text
    off = 0
    while off < len(code):
        for addr, size, mnm, op in md2.disasm_lite(code[off:], img.text_rva + off):
            if (mnm.startswith('j') or mnm == 'call') and op.startswith('0x'):
                t = int(op, 16)
                for (b, e) in ranges:
                    if b <= t < e and not (b <= addr < e) and t != b:
                        ext.setdefault((b, e), set()).add(t)
            off = addr - img.text_rva + size
        off += 1
    for r, t in ext.items():
        print('external entries into %x-%x: %s' % (r[0], r[1], ' '.join('%x' % x for x in sorted(t))))
    for s in sites:
        if trace is not None and s['rva'] != trace:
            continue
        b, e = s['range']
        if (b, e) not in funcs:
            funcs[(b, e)] = Func(img, an.md, b, e)
            funcs[(b, e)].ext = ext.get((b, e), set())
            fl = img.unwind_flags(b)
            funcs[(b, e)].unwind_flags = fl
            funcs[(b, e)].orphans_reachable = bool(funcs[(b, e)].indirect) or bool(fl & 3) or fl < 0
            if funcs[(b, e)].indirect:
                print('indirect jumps in %x-%x: %s' % (b, e, ' '.join('%x' % x for x in funcs[(b, e)].indirect)))
        f = funcs[(b, e)]
        i = f.insns[f.idx[s['rva']]]
        an.notes = []
        cnt = i.operands[2]
        w = 256 if 'ymm' in i.op_str.split(',')[0] else 128
        v = an.opval(f, i, cnt, 0, frozenset(), 32 if w == 256 else 16)
        lanes = 4 if w == 256 else 2
        safe = all(lane_safe(v[2 * q], v[2 * q + 1]) for q in range(lanes))
        kind = 'mem-const' if cnt.type == X.X86_OP_MEM and an.mem_const(i, cnt, 1) is not None else (
            'mem' if cnt.type == X.X86_OP_MEM else 'reg')
        r = dict(rva=s['rva'], indirect_jumps_in_func=len(f.indirect), external_entries=len(f.ext),
                 unwind_flags=f.unwind_flags, bytes=s['bytes'], insn='%s %s' % (i.mnemonic, i.op_str), func=[b, e], count_kind=kind,
                 verdict='SAFE' if safe else 'PATCH',
                 count_dwords=['%x-%x' % x for x in v[:2 * lanes]], notes=sorted(set(an.notes))[:12])
        res.append(r)
        if trace is not None:
            print(json.dumps(r, indent=1))
    if trace is None:
        json.dump(dict(rule='lane bad iff (c>>32)!=0 and (c&0xffffffff)<64', sites=res,
                       n_safe=sum(r['verdict'] == 'SAFE' for r in res), n_patch=sum(r['verdict'] == 'PATCH' for r in res)),
                  open(outp, 'w'), indent=1)
        for r in res:
            print('%8x %-5s %-9s %-40s %s' % (r['rva'], r['verdict'], r['count_kind'], r['insn'], ' '.join(r['count_dwords'])))
        print('SAFE', sum(r['verdict'] == 'SAFE' for r in res), 'PATCH', sum(r['verdict'] == 'PATCH' for r in res))


if __name__ == '__main__':
    main()
