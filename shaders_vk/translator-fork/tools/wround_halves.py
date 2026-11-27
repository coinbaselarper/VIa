"""Exchange rounds the wave64 emulation does not need (translator-fork, --wave-rounds; not in leonida d3b8251b).

Two rules, each proven per instruction by a forward dataflow over the blocks (after waterfall folding):

read alone (v_readfirstlane_b32 sD, vS): the hardware gives every lane the copy of the first lane that is on. When
  every lane that is on holds the same vS ("active-uniform": written by VALU from scalars, constants and
  active-uniform VGPRs, with EXEC only narrowed since), that copy is the lane's own, so the lane reads its own vS and
  no exchange runs. A lane that is off then holds another value in sD; that is exact only when no lane that is off
  can observe it before it is overwritten. The scalars derived from sD (SALU closure, s_load results) are tracked as
  "active-lane only" and the read is refused when one of them reaches a branch condition (SCC, VCC as data), EXEC,
  M0, a scalar store, a v_readlane lane index or a v_writelane, or is live where EXEC may widen. A scalar load whose
  address comes from them loads in lanes that are on only (exec_guard, as for folded waterfalls).

gated votes (s_cbranch_vccz/vccnz, s_cbranch_execz/execnz): LLVM's uniform branches inside divergent code test
  `vcc = exec & s[X]` with `s[X] = s_cselect_b64 exec, 0` (or s_andn2 for the inverse): the bit of every lane that is
  on is SCC at the cselect (one value per wave), so the vote is that SCC whenever some lane is on. The gate (SCC at
  the definition) is kept in a per-register variable; a vote on a gated mask reads it when EXEC is proven non-zero
  (after an s_cbranch_execz fall-through, s_mov_b64 exec, -1, or an EXEC OR since), with EXEC only narrowed since the
  gate was taken. s_and_saveexec_b64 of a gated mask makes EXEC gated the same way (its execz vote is the gate).

    analyse(blocks, succ, raw_edges, lane_masks) -> {"rfl": {id(ins)}, "guard": {id(ins)}, "votes": {id(term)},
                                                     "gate_defs": {id(ins): (dst, how)}, "refused": Counter}
Standard library only.
"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
import wave64_emulate_opt as WO  # noqa: E402
import wave64_halves as WH  # noqa: E402
import lane_masks as LM  # noqa: E402
import wave_uniform as WU  # noqa: E402

BIT_VCC, BIT_SCC, BIT_M0 = 128, 129, 130
NARROW_EXEC = ("s_and_saveexec_b64", "s_andn1_saveexec_b64", "s_andn1_wrexec_b64", "s_and_saveexec_b32",
               "s_andn1_saveexec_b32", "s_andn1_wrexec_b32")
NOT_AU_VALU = WU.NOT_UNIFORM_VALU + ("v_cndmask", "v_addc", "v_subb", "v_add_co_ci", "v_sub_co_ci",
                                      "v_subrev_co_ci", "v_div_fmas", "v_mbcnt", "v_interp", "v_permlane",
                                      "v_readlane", "v_writelane")
LOADS = WU.LOADS
GATE_BIN = ("s_and_b64", "s_or_b64", "s_xor_b64", "s_andn2_b64", "s_orn2_b64", "s_nand_b64", "s_nor_b64", "s_xnor_b64")


def base_op(op):
    return WU.base_op(op)


def _sreg_keys(x):
    return WU._sregs(x)


def _bits(mask):
    out = []
    while mask:
        lsb = mask & -mask
        out.append(lsb.bit_length() - 1)
        mask ^= lsb
    return out


def _is_exec(x):
    return x.get("k") == "special" and x.get("t", "").startswith("exec")


def _is_full_exec_arg(x):
    return x.get("k") == "special" and x.get("t") == "exec"


def _pair_key(x):
    """'vcc', 'exec' or the first SGPR of a 64-bit pair operand; None otherwise."""
    k = x.get("k")
    if k == "special":
        t = x.get("t", "")
        if t == "vcc":
            return "vcc"
        if t == "exec":
            return "exec"
        return None
    if k == "sgpr" and x.get("n", 1) == 2:
        return x["r"]
    return None


class S:
    __slots__ = ("au", "gm", "nz", "al", "aum")

    def __init__(self, au=(), gm=(), nz=False, al=None, aum=()):
        self.au = set(au)
        self.gm = set(gm)
        self.nz = nz
        self.al = dict(al or {})
        self.aum = set(aum)

    def copy(self):
        return S(self.au, self.gm, self.nz, self.al, self.aum)

    def key(self):
        return (frozenset(self.au), frozenset(self.gm), self.nz, frozenset(self.al.items()), frozenset(self.aum))

    def meet(self, o):
        al = dict(self.al)
        for k, v in o.al.items():
            al[k] = al.get(k, frozenset()) | v
        return S(self.au & o.au, self.gm & o.gm, self.nz and o.nz, al, self.aum & o.aum)


class Analysis:
    def __init__(self, blocks, succ, raw_edges, lane_masks, cands):
        self.blocks, self.succ, self.raw_edges, self.lm = blocks, succ, raw_edges, lane_masks
        self.cands = cands          # rfl ids still allowed to read alone (None: first pass, all candidates)
        self.wu = WU.Analysis(lane_masks, WU.LM.analyse(blocks, succ, include_either=True)[2])
        self.wu._after = {}
        for blk in blocks:
            for k, ins in enumerate(blk):
                nxt = blk[k + 1] if k + 1 < len(blk) else None
                self.wu._after[id(ins)] = set(lane_masks.get(id(nxt), {})) if nxt is not None else set()
        self.reset_out()

    def reset_out(self):
        self.rfl, self.guard, self.votes, self.gate_defs = set(), set(), set(), {}
        self.own_bit = set()        # vcc branches whose mask is active-uniform: the lane's own bit inside a fold
        self.viol = collections.Counter()
        self.bad = set()
        self.widen = []             # (id(ins), al taint bits live check later): (block, index, al snapshot)

    # --- facts read from the wave_uniform state ---
    def vgpr_au(self, st, ws, r):
        return r in st.au or r in ws.uv

    def taint_of(self, st, rd):
        t = frozenset()
        for b in _bits(rd):
            v = st.al.get(b)
            if v:
                t |= v
        return t

    def gate_of(self, st, x):
        """The gate of operand x read as a lane mask (its bit in every lane that is on, known in every lane): 'T',
        'F', a gated register key, or None."""
        k = x.get("k")
        if k == "imm":
            v = x["v"] & 0xFFFFFFFF
            return "T" if v == 0xFFFFFFFF else "F" if v == 0 else None
        if _is_full_exec_arg(x):
            return "T"
        key = _pair_key(x)
        return key if key is not None and key in st.gm else None

    def mask_aum(self, st, ws, ins, x):
        """Operand x, read as a lane mask, has one bit in every lane that is on."""
        k = x.get("k")
        if k == "imm":
            return True
        if k == "special" and x.get("t") == "exec":
            return True
        key = _pair_key(x)
        if key is None:
            return False
        if key in st.aum or key in st.gm:
            return True
        regs = _sreg_keys(x)
        return bool(regs) and all(r in ws.lu for r in regs)

    def aum_step(self, st, ws, ins, op, a, may_w, narrowing_exec):
        """Active-uniform lane masks after ins (EXEC writes are handled by the caller)."""
        new = None
        if op.startswith("v_cmp") and not op.startswith("v_cmpx") and not ins["op"].endswith(("_dpp", "_sdwa")):
            srcs = a[1:] if len(a) >= 3 else a
            dst = _pair_key(a[0]) if len(a) >= 3 else "vcc"
            ok = all(self.val_au(st, ws, ins, x) for x in srcs)
            new = (dst, ok)
        elif op == "s_cselect_b64" and len(a) == 3:
            ok = "scc" not in ws.ns and all(self.mask_aum(st, ws, ins, x) for x in a[1:])
            new = (_pair_key(a[0]), ok)
        elif op in ("s_and_b64", "s_or_b64", "s_xor_b64", "s_andn2_b64", "s_orn2_b64", "s_nand_b64", "s_nor_b64",
                    "s_xnor_b64") and len(a) == 3:
            new = (_pair_key(a[0]), all(self.mask_aum(st, ws, ins, x) for x in a[1:]))
        elif op in ("s_mov_b64", "s_not_b64") and len(a) == 2:
            new = (_pair_key(a[0]), self.mask_aum(st, ws, ins, a[1]))
        elif "_saveexec_b64" in op and a:
            new = (_pair_key(a[0]), True)
        for x in _bits(may_w):
            if x == BIT_VCC:
                st.aum.discard("vcc")
            elif x < 128:
                st.aum.discard(x)
                st.aum.discard(x - 1)
        if new is not None and new[0] is not None and new[0] != "exec" and new[1]:
            st.aum.add(new[0])

    def val_au(self, st, ws, ins, x):
        kx = x.get("k")
        if kx == "vgpr":
            return all(self.vgpr_au(st, ws, r) for r in range(x["r"], x["r"] + x.get("n", 1)))
        if kx in ("sgpr", "special", "slist"):
            for r in _sreg_keys(x):
                if r == "exec" or self.wu.is_mask(ins, r) or r in ws.ns:
                    return False
            return bool(_sreg_keys(x)) or (kx == "special" and x.get("t") == "null")
        return kx in ("imm", "fimm", "lit", "literal", "const")

    def violate(self, t, why):
        if t:
            self.bad |= t
            self.viol[why] += 1

    def step(self, st, ws, ins, b, k):
        op = base_op(ins["op"])
        raw = ins["op"]
        a = ins["args"]
        rd, may_w, kill = WO._scalar_rw(ins)
        taint = self.taint_of(st, rd)
        dst_exec = op.startswith("s_") and op not in WU.BRANCHES and not op.startswith(("s_cmp", "s_bitcmp")) and (
            (bool(a) and _is_exec(a[0])) or "_saveexec_" in op or "_wrexec_" in op)
        # ---- terminators: votes ----
        if op in WU.BRANCHES:
            if op in ("s_cbranch_scc0", "s_cbranch_scc1"):
                self.violate(taint, "branch-scc")
            elif op in ("s_cbranch_vccz", "s_cbranch_vccnz"):
                self.violate(self.taint_of(st, 1 << BIT_VCC), "branch-vcc")
                if "vcc" in st.gm and st.nz:
                    self.votes.add(id(ins))
                if "vcc" in st.aum:
                    self.own_bit.add(id(ins))
            elif op in ("s_cbranch_execz", "s_cbranch_execnz"):
                if "exec" in st.gm:
                    self.votes.add(id(ins))
            return
        if op == "s_endpgm":
            return
        # ---- v_readfirstlane ----
        if op == "v_readfirstlane_b32" and a[1].get("k") == "vgpr" and ("v", a[1]["r"]) not in self.lm.get(id(ins), {}):
            r = a[1]["r"]
            dst = _bits(may_w)
            if ins.get("own_lane") or r in ws.uv:
                for x in dst:
                    st.al.pop(x, None)
            elif r in st.au and (self.cands is None or id(ins) in self.cands):
                self.rfl.add(id(ins))
                for x in dst:
                    st.al[x] = frozenset([id(ins)])
            else:
                for x in dst:
                    st.al.pop(x, None)
            for x in dst:
                if isinstance(x, int):
                    st.gm.discard(x)
                    st.aum.discard(x)
                    st.aum.discard(x - 1)
            return
        # ---- EXEC writes ----
        if dst_exec or op.startswith("v_cmpx"):
            src_keys = [_pair_key(x) for x in a[1:]]
            narrowing = op.startswith("v_cmpx") or op in NARROW_EXEC or (
                op in ("s_and_b64", "s_and_b32") and any(_is_exec(x) for x in a[1:3])) or (
                op in ("s_andn2_b64", "s_andn2_b32") and len(a) > 1 and _is_exec(a[1]))
            self.violate(taint, "exec-from-active-only")
            gated_src = None
            if op in ("s_and_saveexec_b64", "s_and_b64") and st.nz:
                others = [x for x in a[1:3] if not _is_exec(x)]
                if len(others) == 1 and _pair_key(others[0]) in st.gm:
                    gated_src = _pair_key(others[0])
            if "_saveexec_" in op and a and _pair_key(a[0]) is not None:
                st.gm.discard(_pair_key(a[0]))
            self.aum_step(st, ws, ins, op, a, may_w, True)
            if narrowing:
                st.nz = False
                st.gm.discard("exec")
                if gated_src is not None:
                    st.gm.add("exec")
                    self.gate_defs[id(ins)] = ("exec", ("s_mov_b64", gated_src, None))
            else:
                full = op == "s_mov_b64" and len(a) > 1 and a[1].get("k") == "imm" and \
                    (a[1]["v"] & 0xFFFFFFFF) == 0xFFFFFFFF
                keep_nz = st.nz and op == "s_or_b64" and any(_is_exec(x) for x in a[1:3])
                st.nz = full or keep_nz
                st.au.clear()
                st.gm.clear()
                st.aum.clear()
                self.widen.append((b, k))
            for x in _bits(may_w):
                if x != BIT_SCC:
                    st.al.pop(x, None)
            if taint:
                st.al[BIT_SCC] = taint
            else:
                st.al.pop(BIT_SCC, None)
            return
        # ---- scalar ALU / memory ----
        if op.startswith("s_"):
            if op.startswith(("s_store", "s_buffer_store", "s_atomic", "s_buffer_atomic", "s_dcache_inv",
                              "s_sendmsg", "s_setreg", "s_movrel", "s_setpc", "s_swappc", "s_cbranch_g")):
                self.violate(taint, "scalar-side-effect")
            if op.startswith("s_movrel") or (rd >> BIT_M0) & 1 and op.startswith(("s_sendmsg", "s_ttrace")):
                self.violate(self.taint_of(st, 1 << BIT_M0), "m0")
            if op.startswith(("s_load", "s_buffer_load")) and taint:
                self.guard.add(id(ins))
            self.aum_step(st, ws, ins, op, a, may_w, False)
            dk = _pair_key(a[0]) if a else None
            gate = None
            if dk is not None and dk != "exec" and not taint and len(a) in (2, 3):
                g = [self.gate_of(st, x) for x in a[1:]]
                if op == "s_cselect_b64" and len(a) == 3 and "scc" not in ws.ns and None not in g:
                    gate = ("cselect", g[0], g[1])
                elif op in GATE_BIN and len(a) == 3 and None not in g:
                    gate = (op, g[0], g[1])
                elif op in ("s_mov_b64", "s_not_b64") and len(a) == 2 and None not in g:
                    gate = (op, g[0], None)
            for x in _bits(may_w):
                if isinstance(x, int) and x < 128:
                    st.gm.discard(x)
                    st.gm.discard(x - 1)
                elif x == BIT_VCC:
                    st.gm.discard("vcc")
            if gate is not None:
                st.gm.add(dk)
                self.gate_defs[id(ins)] = (dk, gate)
            for x in _bits(kill):
                st.al.pop(x, None)
            if taint:
                for x in _bits(may_w):
                    st.al[x] = st.al.get(x, frozenset()) | taint
            return
        # ---- vector ALU ----
        if op.startswith("v_"):
            if op.startswith("v_writelane"):
                self.violate(taint, "writelane")
            if op.startswith("v_readlane") and len(a) > 2:
                self.violate(self.taint_of(st, WO._operand_scalar_mask(a[2])), "readlane-index")
            if op.startswith("v_movrel"):
                self.violate(self.taint_of(st, 1 << BIT_M0), "m0")
            mods = ins.get("mods", ())
            cross = op.startswith(NOT_AU_VALU) or raw.endswith(("_dpp", "_sdwa")) or any(
                m.startswith(("quad_perm", "row_", "wave_", "dpp8", "dst_sel", "op_sel")) for m in mods)
            two = op in WU.TWO_DEST and len(a) > 2 and a[1].get("k") in ("sgpr", "special", "slist")
            srcs = a[2:] if two else a[1:]
            ok = not cross
            for x in srcs:
                kx = x.get("k")
                if kx == "vgpr":
                    ok = ok and all(self.vgpr_au(st, ws, r) for r in range(x["r"], x["r"] + x.get("n", 1)))
                elif kx in ("sgpr", "special", "slist"):
                    for r in _sreg_keys(x):
                        if r == "exec" or self.wu.is_mask(ins, r) or r in ws.ns:
                            ok = False
                elif kx not in ("imm", "fimm", "lit", "literal", "const"):
                    ok = False
            self.aum_step(st, ws, ins, op, a, may_w, False)
            cmp_gate = None
            if op.startswith("v_cmp_") and not op.startswith("v_cmp_class") and not cross and not taint:
                csrc = a[1:] if len(a) >= 3 else a
                if all(self.wu.val_u(ws, ins, x) for x in csrc) and all(x.get("k") != "vgpr" or all(
                        r in ws.uv for r in range(x["r"], x["r"] + x.get("n", 1))) for x in csrc):
                    cmp_gate = _pair_key(a[0]) if len(a) >= 3 else "vcc"
            if a and a[0].get("k") == "vgpr":
                for r in range(a[0]["r"], a[0]["r"] + a[0].get("n", 1)):
                    if ok:
                        st.au.add(r)
                    else:
                        st.au.discard(r)
            for x in _bits(may_w):
                st.al.pop(x, None)
                if x == BIT_VCC:
                    st.gm.discard("vcc")
                elif x < 128:
                    st.gm.discard(x)
                    st.gm.discard(x - 1)
            if cmp_gate is not None and cmp_gate != "exec":
                st.gm.add(cmp_gate)
                self.gate_defs[id(ins)] = (cmp_gate, ("cmp", None, None))
            return
        # ---- vector memory, LDS, exports ----
        if op.startswith("ds_") or op.startswith("exp"):
            self.violate(self.taint_of(st, 1 << BIT_M0), "m0")
        if a:
            for x in a[:1]:
                if x.get("k") == "vgpr" and op.startswith(LOADS):
                    for r in range(x["r"], x["r"] + x.get("n", 1)):
                        st.au.discard(r)
        if op.startswith(("buffer_atomic", "global_atomic", "flat_atomic", "image_atomic", "ds_")):
            for x in a[:2]:
                if x.get("k") == "vgpr":
                    for r in range(x["r"], x["r"] + x.get("n", 1)):
                        st.au.discard(r)
        for x in _bits(may_w):
            st.al.pop(x, None)
            if x == BIT_VCC:
                st.aum.discard("vcc")
            elif x < 128:
                st.aum.discard(x)
                st.aum.discard(x - 1)

    def run(self):
        blocks, succ, raw_edges = self.blocks, self.succ, self.raw_edges
        n = len(blocks)
        if not n:
            return
        entry = [None] * n
        wentry = [None] * n
        entry[0] = S(nz=True)
        wentry[0] = WU.State(lu={"exec"} | set(range(128)) | {"vcc"}, full={"exec"})
        work = [0]
        while work:
            b = work.pop()
            st, ws = entry[b].copy(), wentry[b].copy()
            self.reset_out()
            for k, ins in enumerate(blocks[b]):
                self.step(st, ws, ins, b, k)
                self.wu.step(ws, ins)
            for i, s in enumerate(succ[b]):
                st2 = self.edge(st, b, s)
                m = st2 if entry[s] is None else entry[s].meet(st2)
                wm = ws if wentry[s] is None else wentry[s].meet(ws)
                if entry[s] is None or m.key() != entry[s].key() or wm.key() != wentry[s].key():
                    entry[s], wentry[s] = m, wm
                    work.append(s)
        self.reset_out()
        self.entry, self.wentry = entry, wentry
        for b in range(n):
            if entry[b] is None:
                continue
            st, ws = entry[b].copy(), wentry[b].copy()
            for k, ins in enumerate(blocks[b]):
                self.step(st, ws, ins, b, k)
                self.wu.step(ws, ins)
        self.check_widen()

    def edge(self, st, b, s):
        blk = self.blocks[b]
        term = next((i for i in blk if base_op(i["op"]) in WU.BRANCHES), None)
        if term is None:
            return st
        op = base_op(term["op"])
        edges = self.raw_edges[b]
        tgt = edges[0]
        fall = edges[1] if len(edges) > 1 else None
        out = st
        if op == "s_cbranch_execz" and s == fall and s != tgt:
            out = st.copy()
            out.nz = True
        elif op == "s_cbranch_execnz" and s == tgt and s != fall:
            out = st.copy()
            out.nz = True
        if op in ("s_cbranch_execz", "s_cbranch_execnz") and "exec" in out.gm:
            if out is st:
                out = st.copy()
            out.gm.discard("exec")
        return out

    def check_widen(self):
        """An active-lane-only scalar live just after an EXEC write that may widen EXEC: a lane that comes on would
        read the value it holds, not the wave's."""
        if not self.widen:
            return
        live_in = _liveness(self.blocks, self.succ)
        for b, k in self.widen:
            blk = self.blocks[b]
            live = 0
            for s in self.succ[b]:
                live |= live_in[s]
            for ins in reversed(blk[k + 1:]):
                rd, _, mk = _rw(ins)
                live = rd | (live & ~mk)
            st = self.entry[b].copy()
            # taint just after instruction k: replay the block prefix (state only, no outputs kept)
            saved = (self.rfl, self.guard, self.votes, self.gate_defs, self.viol, self.bad, self.widen)
            self.rfl, self.guard, self.votes, self.gate_defs = set(), set(), set(), {}
            self.viol, self.bad, self.widen = collections.Counter(), set(), []
            ws = self.wentry[b].copy()
            for ins in blk[:k + 1]:
                self.step(st, ws, ins, b, 0)
                self.wu.step(ws, ins)
            self.rfl, self.guard, self.votes, self.gate_defs, self.viol, self.bad, self.widen = saved
            t = self.taint_of(st, live)
            self.violate(t, "live-at-exec-widen")


def analyse(blocks, succ, raw_edges, lane_masks):
    cands = None
    refused = collections.Counter()
    for _ in range(64):
        an = Analysis(blocks, succ, raw_edges, lane_masks, cands)
        an.run()
        if not an.bad:
            break
        refused.update(an.viol)
        cands = an.rfl - an.bad
    return {"rfl": an.rfl, "guard": an.guard, "votes": an.votes, "gate_defs": an.gate_defs, "refused": refused,
            "own_bit": an.own_bit}


# ---- execz fall-through: a skip branch whose region is a no-op for a wave with no lane on ----
# s_cbranch_execz over region R (the blocks reached from the fall-through before its post-dominator `skip`) is only
# a shortcut when R, run with EXEC = 0, changes nothing `skip` and later code can see: every VALU, VMEM, LDS and
# export of R is masked (no lane is on); R's scalar writes are dead at `skip`; EXEC stays 0 (only narrowing writes or
# restores of masks R saved itself, wave64_emulate_opt._exec_writes_stay_inactive); no unmasked side effect
# (barrier, message, scalar store, v_writelane, GDS/ordered counts, end of program) is in R. Then the wave may run R
# instead of voting, as hardware would without the shortcut. Scalar loads inside R whose address comes from a value R
# reads from a VGPR or a lane mask (garbage when no lane is on) refuse the rule: that load could leave the title's
# memory, which the console never does because it skips. Every exit of R's loops is then the console's own exit.

def _rw(ins):
    """wave64_emulate_opt._scalar_rw, except that the VOP3 (_e64) forms of v_cndmask / carry-in ops read the mask
    operand they name, not VCC (the shared table counts the e32 form's implicit VCC read for both)."""
    rd, mw, kill = WO._scalar_rw(ins)
    raw = ins["op"]
    if raw.endswith("_e64") and WO.base_op(raw) in WO._IMPLICIT_VCC_READS and not any(
            x.get("k") == "special" and x.get("t", "").startswith("vcc") for x in ins["args"][1:]):
        rd &= ~(1 << BIT_VCC)
    return rd, mw, kill


def _liveness(blocks, succ):
    n = len(blocks)
    preds = [[] for _ in range(n)]
    for b in range(n):
        for x in succ[b]:
            preds[x].append(b)
    use, kill = [0] * n, [0] * n
    for b, blk in enumerate(blocks):
        u = k = 0
        for ins in WO._active_insns(blk):
            rd, _, mk = _rw(ins)
            u |= rd & ~k
            k |= mk
        use[b], kill[b] = u, k
    return WO._solve_scalar_liveness(use, kill, preds)


FT_FORBID = ("s_barrier", "s_sendmsg", "s_store", "s_buffer_store", "s_atomic", "s_buffer_atomic", "s_dcache",
             "v_writelane", "ds_gws", "ds_ordered", "ds_append", "ds_consume", "s_setreg", "s_trap", "s_sethalt",
             "s_setpc", "s_swappc", "s_movrel", "exp", "s_cbranch_g", "s_icache", "s_ttrace", "v_movrel")


def _region_of(b, run, skip, succ):
    region, work = set(), [run]
    while work:
        u = work.pop()
        if u == skip or u in region:
            continue
        if u == b:
            return None
        region.add(u)
        work.extend(x for x in succ[u] if x != skip and x not in region)
    return region


def fallthrough_heads(blocks, succ, raw_edges, lane_masks, mask_writes):
    n = len(blocks)
    if n < 2:
        return set(), {}
    ipdom = WO._postdominators(succ)
    live_in = _liveness(blocks, succ)
    info, terms = [None] * n, [None] * n
    for u, blk in enumerate(blocks):
        bad, may_w, exec_writes, sgpr_ops, taint_load = None, 0, [], [], False
        tainted = 0
        for ins in blk:
            m = WO._get_op_meta(ins["op"])
            op = m[0]
            a = ins["args"]
            if m[1]:
                terms[u] = op
                break
            if id(ins) in mask_writes:
                bad = bad or "parked-mask"
            if op.startswith(FT_FORBID):
                bad = bad or "side-effect:" + op
            rd, mw, kill = WO._scalar_rw_with_meta(ins, m)
            if op.startswith(("s_load", "s_buffer_load")) and not ins.get("exec_guard"):
                base_bits = WO._operand_scalar_mask(a[1]) if len(a) > 1 else 0
                off_bits = WO._operand_scalar_mask(a[2]) if len(a) > 2 and op.startswith("s_load") else 0
                if tainted & (base_bits | off_bits):
                    taint_load = True
            if op.startswith("v_") or op.startswith(("s_bcnt", "s_ff", "s_flbit", "s_quadmask", "s_wqm",
                                                     "s_bitreplicate")) or (rd & tainted and op.startswith("s_")):
                tainted |= mw
            else:
                tainted &= ~kill
            may_w |= mw
            s_bits = mw & WO._ALL_SGPRS
            if s_bits:
                sgpr_ops.append((s_bits, op, a))
            if (m[8] & WO._FLAG_WRITES_EXEC_OP) or (bool(a) and a[0].get("k") == "special"
                                                     and a[0].get("t", "").startswith("exec")):
                exec_writes.append((op, a))
        if terms[u] is None:
            terms[u] = "fall"
        if terms[u] == "s_endpgm":
            bad = bad or "end-of-program"
        if taint_load:
            bad = bad or "scalar-load-from-vector-value"
        info[u] = (bad, None, may_w, exec_writes, sgpr_ops)
    heads, why = set(), collections.Counter()
    for b in range(n):
        if terms[b] != "s_cbranch_execz" or len(raw_edges[b]) < 2:
            continue
        tgt, fall = raw_edges[b][0], raw_edges[b][1]
        run, skip = fall, tgt
        if run == skip or ipdom.get(b) != skip:
            why["not-postdominated"] += 1
            continue
        region = _region_of(b, run, skip, succ)
        if not region or 0 in region:
            why["cycle-through-head"] += 1
            continue
        bad = next((info[u][0] for u in region if info[u][0]), None)
        if bad:
            why[bad] += 1
            continue
        mw = 0
        for u in region:
            mw |= info[u][2]
        if mw & live_in[skip]:
            hit = mw & live_in[skip]
            why["live-at-skip:" + ("vcc" if hit >> 128 & 1 else "") + ("scc" if hit >> 129 & 1 else "") +
                ("m0" if hit >> 130 & 1 else "") + ("s" if hit & WO._ALL_SGPRS else "")] += 1
            continue
        if not WO._exec_writes_stay_inactive(region, info, live_in[run]):
            why["exec-may-widen"] += 1
            continue
        # A loop of R must leave when no lane is on: its conditional branches may only be execnz / vccnz (both fall
        # through with EXEC = 0); a vccz / execz / SCC test could wait for a lane forever.
        if WO._region_has_cycle(region, succ) and any(
                terms[u] in ("s_cbranch_vccz", "s_cbranch_execz", "s_cbranch_scc0", "s_cbranch_scc1") for u in region):
            why["loop-may-wait-for-a-lane"] += 1
            continue
        heads.add(b)
    return heads, why


# ---- translator hooks (rdna2spirv.py: main -> add_arguments/configure, translate -> apply, branch loop -> gated,
# branch_condition -> gate_cond) ----

WAVE_ROUNDS = False
HALF_SHUFFLE = False
IN_HALF_OPS = ("ds_swizzle_b32", "ds_bpermute_b32", "v_permlane16_b32", "v_permlanex16_b32")


def add_arguments(ap):
    ap.add_argument("--wave-rounds", action="store_true",
                    help="translator-fork (tools/wround_halves.py): under --wave64-emulate, read-alone "
                         "v_readfirstlane and gated uniform-branch votes run without an exchange round")
    ap.add_argument("--half-shuffle", action="store_true",
                    help="translator-fork (tools/wround_halves.py): under --wave64-emulate, DPP, ds_swizzle, "
                         "ds_bpermute and v_permlane(x)16, which never cross a 32-lane half, are SIMD-group shuffles "
                         "(console lane = 32 x SIMD group + SIMD lane, whole waves only): no exchange round")
    ap.add_argument("--spill-lanes", action="store_true",
                    help="translator-fork (tools/wround_halves.py): under --wave64-emulate, a VGPR used only as "
                         "constant-lane SGPR spill storage (v_writelane / v_readlane, wave64_halves' pure-spill proof) "
                         "keeps each lane slot as a scalar: v_readlane of it runs without an exchange round")
    ap.add_argument("--heap-tindex", action="store_true",
                    help="translator-fork (tools/wround_halves.py): under --wave64-emulate, a T# loaded from the "
                         "console heap takes its bindless index from the layer's per-slot table (hash as fallback); "
                         "only for a layer that publishes that table")


def configure(a):
    global WAVE_ROUNDS, HEAP_TINDEX, HALF_SHUFFLE, SPILL_LANES
    WAVE_ROUNDS = bool(getattr(a, "wave_rounds", False))
    WO.E64_MASK_EXACT = WAVE_ROUNDS
    HALF_SHUFFLE = bool(getattr(a, "half_shuffle", False))
    HEAP_TINDEX = bool(getattr(a, "heap_tindex", False))
    SPILL_LANES = bool(getattr(a, "spill_lanes", False))


SPILL_LANES = False


MOVREL_OPS = ("v_movrels_b32", "v_movreld_b32", "v_movrelsd_b32", "v_movrelsd_2_b32", "v_swaprel_b32")


def park_spills(g, blocks, succ):
    """SGPR spill slots in VGPR lanes. A VGPR that only v_writelane / v_readlane with an immediate lane touch (no vector
    operand use, no v_movrel* reach) is a set of independent lane slots; a slot whose every write is an SGPR or
    constant that is no lane mask (the lane-mask analysis' parked masks keep their own path) holds one scalar per
    wave, written independently of EXEC and read back by the whole wave. Every invocation keeps it as a parked SGPR:
    the same value the emulation would fetch from that lane's invocation with an exchange round."""
    bad, mask_slots, uses = set(), set(), collections.defaultdict(list)
    movrel_min = None
    for blk in blocks:
        for ins in blk:
            op, a = base_op(ins["op"]), ins["args"]
            st = g.lane_masks.get(id(ins), {})
            if op in MOVREL_OPS:
                for x in a:
                    if x.get("k") == "vgpr":
                        movrel_min = x["r"] if movrel_min is None else min(movrel_min, x["r"])
            if (op == "v_writelane_b32" and len(a) == 3 and a[0].get("k") == "vgpr" and a[0].get("n", 1) == 1
                    and a[2].get("k") == "imm" and a[1].get("k") != "vgpr"):
                s = (a[0]["r"], a[2]["v"] & 63)
                if (("lane",) + s in st or (a[1].get("k") == "special" and a[1].get("t", "").startswith("exec"))
                        or LM.kind(st, a[1]) in LM.MASK_KINDS):
                    mask_slots.add(s)
                uses[s].append(ins)
                continue
            if (op == "v_readlane_b32" and len(a) == 3 and a[1].get("k") == "vgpr" and a[1].get("n", 1) == 1
                    and a[2].get("k") == "imm"):
                s = (a[1]["r"], a[2]["v"] & 63)
                if ("lane",) + s in st:
                    mask_slots.add(s)
                uses[s].append(ins)
                continue
            for x in a:
                for r in WH._regs(x):
                    if isinstance(r, tuple) and r[0] == "v":
                        bad.add(r[1])
    ids = set()
    for s, insns in uses.items():
        if s in mask_slots or s[0] in bad or (movrel_min is not None and s[0] >= movrel_min):
            continue
        if s not in g.parked:
            g.parked[s] = g.SGPRS + len(g.parked)
        g.w64_scalar_spills.add(s)
        for ins in insns:
            ins["lane_local_ok"] = True
            ids.add(id(ins))
    g.wr_spills = len(ids)
    return frozenset(ids)


def _var(key):
    return "%wg_vcc" if key == "vcc" else "%wg_exec" if key == "exec" else f"%wg_s{key}"


def apply(g, blocks, succ, raw_edges):
    """Run the analysis for an emulated whole-wave shader; mark the instructions; returns the ids to add to
    wave64_emulate_opt.UNIFORM_IDS."""
    g.wr = None
    g.wr_in_half, g.wr_shuffles = False, 0
    if HEAP_TINDEX and g.emulate:
        install_heap_tindex(g)
    spec = g.cs or g.ms
    size = spec["size"] if spec else None
    whole = bool(g.emulate and g.wave64 and size and g.x_lanes == size[0] * size[1] * size[2])
    g.wr_whole = whole
    spills = park_spills(g, blocks, succ) if (SPILL_LANES and whole) else frozenset()
    if HALF_SHUFFLE and whole and not WAVE_ROUNDS:
        install_insn(g, {"gate_defs": {}})
    if not (WAVE_ROUNDS and whole):
        return spills
    if not g.wu_votes and not g.wu_rfl:
        wu = WU.analyse(blocks, succ, g.lane_masks)
        g.wu_votes, g.wu_rfl = frozenset(wu["votes"]), frozenset(wu["rfl"])
    r = analyse(blocks, succ, raw_edges, g.lane_masks)
    heads, r["ft_refused"] = fallthrough_heads(blocks, succ, raw_edges, g.lane_masks, g.mask_writes)
    r["fallthrough"] = set()
    for b in heads:
        term = next(i for i in blocks[b] if WO._get_op_meta(i["op"])[1])
        r["fallthrough"].add(id(term))
    for blk in blocks:
        for ins in blk:
            if id(ins) in r["rfl"]:
                ins["own_lane"] = True
            if id(ins) in r["guard"]:
                ins["exec_guard"] = True
            if ins.get("own_lane"):
                ins["lane_local_ok"] = True
            if id(ins) in r["own_bit"]:
                ins["fold_own_bit"] = True
    g.wr = r
    install_insn(g, r)
    return frozenset(r["rfl"]) | frozenset(r["votes"]) | frozenset(r["fallthrough"]) | g.wu_votes | g.wu_rfl | spills


def install_insn(g, r):
    keys = {d[0] for d in r["gate_defs"].values()}
    for k in sorted(keys, key=str):
        g.eager_vars.append((_var(k), "%_ptr_Function_bool"))
    orig = g.insn

    def gval(x):
        if x == "T":
            return "%true"
        if x == "F":
            return "%false"
        v = g.tmp()
        g.emit(f"{v} = OpLoad %bool {_var(x)}")
        return v

    def logic(op, x, y):
        b = g.tmp()
        if op == "s_and_b64":
            g.emit(f"{b} = OpLogicalAnd %bool {x} {y}")
        elif op == "s_or_b64":
            g.emit(f"{b} = OpLogicalOr %bool {x} {y}")
        elif op == "s_xor_b64":
            g.emit(f"{b} = OpLogicalNotEqual %bool {x} {y}")
        elif op == "s_xnor_b64":
            g.emit(f"{b} = OpLogicalEqual %bool {x} {y}")
        else:
            ny = g.tmp()
            if op in ("s_andn2_b64", "s_orn2_b64"):
                g.emit(f"{ny} = OpLogicalNot %bool {y}")
                g.emit(f"{b} = {'OpLogicalAnd' if op == 's_andn2_b64' else 'OpLogicalOr'} %bool {x} {ny}")
            else:
                g.emit(f"{ny} = {'OpLogicalAnd' if op == 's_nand_b64' else 'OpLogicalOr'} %bool {x} {y}")
                g.emit(f"{b} = OpLogicalNot %bool {ny}")
        return b

    def insn(ins):
        d = r["gate_defs"].get(id(ins))
        pre = None
        if d is not None and d[1][0] not in ("cmp",):
            dst, (how, x, y) = d
            # operand gates are read before the instruction writes its destination (it may be a source)
            if how == "cselect":
                s, c = g.tmp(), g.tmp()
                g.emit(f"{s} = OpLoad %uint %scc")
                g.emit(f"{c} = OpINotEqual %bool {s} {g.cu(0)}")
                pre = g.tmp()
                g.emit(f"{pre} = OpSelect %bool {c} {gval(x)} {gval(y)}")
            elif how == "s_mov_b64":
                pre = gval(x)
            elif how == "s_not_b64":
                pre = g.tmp()
                g.emit(f"{pre} = OpLogicalNot %bool {gval(x)}")
            else:
                pre = logic(how, gval(x), gval(y))
        if d is not None and d[1][0] == "cmp":
            g.wr_cmp_gate = d[0]
        bop = base_op(ins["op"])
        g.wr_in_half = HALF_SHUFFLE and g.wr_whole and (bop in IN_HALF_OPS or ins["op"].endswith("_dpp") or any(
            m.startswith(("quad_perm", "row_", "dpp8")) for m in ins.get("mods", ())))
        orig(ins)
        g.wr_in_half = False
        g.wr_cmp_gate = None
        if pre is not None:
            g.emit(f"OpStore {_var(d[0])} {pre}")
    g.insn = insn
    g.wr_cmp_gate = None
    orig_wcr = g.write_compare_result

    def write_compare_result(b, dst, scalar, exec_only):
        if g.wr_cmp_gate is not None and not scalar and not exec_only:
            g.emit(f"OpStore {_var(g.wr_cmp_gate)} {b}")
        return orig_wcr(b, dst, scalar, exec_only)
    g.write_compare_result = write_compare_result


def half_shuffle(g):
    """The current instruction is a cross-lane op that stays inside its 32-lane half, run as SIMD-group ops."""
    if getattr(g, "wr_in_half", False):
        g.wr_shuffles += 1
        return True
    return False


def shuffle_half(g, r, v, src):
    i, sh = g.tmp(), g.tmp()
    g.emit(f"{i} = OpBitwiseAnd %uint {src} {g.cu(31)}")
    g.emit(f"{sh} = OpGroupNonUniformShuffle %uint {g.cu(3)} {v} {i}")
    g.emit(f"{r} = OpCopyObject %uint {g.eager('%uint', sh)}")


def shuffle_ok(g, line):
    return getattr(g, "wr_shuffles", 0) > 0 and (
        "%sg_lane" in line or any(o in line for o in ("OpGroupNonUniformShuffle ", "OpGroupNonUniformBallot ",
                                                      "OpGroupNonUniformBallotBitExtract ")))


def keep_shuffle_caps(g):
    if getattr(g, "wr_shuffles", 0) > 0:
        g.uses_subgroup = g.uses_ballot = True


def gated(g, term):
    """Whether the block terminator `term` is a gated vote; arms gate_cond for the block loop's branch_condition."""
    wr = getattr(g, "wr", None)
    g.pending_gate = id(term) if (wr and (id(term) in wr["votes"] or id(term) in wr["fallthrough"])) else None
    g.pending_fallthrough = bool(wr) and id(term) in wr["fallthrough"]
    return g.pending_gate is not None


def gate_cond(g, op):
    """Boolean id for an armed gated vote (the branch's taken condition), or None. Only the block loop's call, which
    runs with cur_uniform_vote set, takes it."""
    if not g.cur_uniform_vote or getattr(g, "pending_gate", None) is None:
        return None
    g.pending_gate = None
    if getattr(g, "pending_fallthrough", False) and op == "s_cbranch_execz":
        g.pending_fallthrough = False
        return "%false"
    key = "vcc" if op in ("s_cbranch_vccz", "s_cbranch_vccnz") else "exec"
    v = g.tmp()
    g.emit(f"{v} = OpLoad %bool {_var(key)}")
    if op in ("s_cbranch_vccnz", "s_cbranch_execnz"):
        return v
    b = g.tmp()
    g.emit(f"{b} = OpLogicalNot %bool {v}")
    return b


# ---- console-heap T# by slot index (--heap-tindex; needs the layer's slot table, docs: wave-rounds STATUS) ----
# The descriptor table buffer (binding 2: word 0 the hash mask M, then (M + 1) slots of 10 words) is followed by a
# header at word H = 1 + (M + 1) * 10: [HEAP_MAGIC, heap console VA lo, hi, slot count N, word offset T of the slot
# table]. Entry s of the table (9 words at T + 9 s) holds the 8 T# words the layer bound for heap bytes
# [32 s, 32 s + 32) and their bindless index (0: none). A T# loaded by s_load_dwordx8 from inside the heap compares
# the loaded words with its entry and takes the index on a match; anything else goes through the hash probe.

HEAP_TINDEX = False
HEAP_MAGIC = 0x31585448


def install_heap_tindex(g):
    orig_load, orig_lookup = g.scalar_load, g.desc_lookup_sgpr
    g.wr_taddr = {}

    def scalar_load(op, a, mods):
        addr = None
        if (g.emulate and base_op(op) == "s_load_dwordx8" and len(a) == 3 and a[0].get("k") == "sgpr"
                and a[1].get("k") == "sgpr" and a[1].get("n", 1) == 2 and not any(m.startswith("offset:") for m in mods)):
            off = a[2]
            o64 = None
            if off.get("k") == "imm":
                imm = ((off["v"] & 0x1FFFFF) ^ 0x100000) - 0x100000
                o64 = g.cl((imm & ~3) & 0xFFFFFFFFFFFFFFFF)
            elif off.get("k") == "special" and off.get("t") == "null":
                o64 = g.cl(0)
            elif off.get("k") == "sgpr" and off.get("n", 1) == 1:
                r4, r64 = g.tmp(), g.tmp()
                g.emit(f"{r4} = OpBitwiseAnd %uint {g.load_reg('sgpr', off['r'])} {g.cu(0xFFFFFFFC)}")
                g.emit(f"{r64} = OpUConvert %ulong {r4}")
                o64 = r64
            if o64 is not None:
                lo, hi = g.load_reg("sgpr", a[1]["r"]), g.load_reg("sgpr", a[1]["r"] + 1)
                lo4, l64, h64, sh, base, full = (g.tmp() for _ in range(6))
                g.emit(f"{lo4} = OpBitwiseAnd %uint {lo} {g.cu(0xFFFFFFFC)}")
                g.emit(f"{l64} = OpUConvert %ulong {lo4}")
                g.emit(f"{h64} = OpUConvert %ulong {hi}")
                g.emit(f"{sh} = OpShiftLeftLogical %ulong {h64} %c64_32")
                g.emit(f"{base} = OpBitwiseOr %ulong {sh} {l64}")
                g.emit(f"{full} = OpIAdd %ulong {base} {o64}")
                addr = full
        orig_load(op, a, mods)
        if addr is not None:
            d = a[0]["r"]
            g.wr_taddr[d] = (addr, tuple(g.x_tok.get(("sgpr", d + i)) for i in range(8)), len(g.x_marks),
                             g.block_label, g.x_gen)

    def desc_lookup_sgpr(a, n):
        rec = g.wr_taddr.get(a.get("r")) if (n == 8 and a.get("k") == "sgpr") else None
        if rec is None or (a["r"], 8) in g.desc_cache or rec[1] != tuple(
                g.x_tok.get(("sgpr", a["r"] + i)) for i in range(8)) or rec[2:] != (len(g.x_marks), g.block_label, g.x_gen):
            return orig_lookup(a, n)
        top = g.desc_cache_ok()
        words = g.sgpr_words(a, 8)
        idx = slot_lookup(g, rec[0], words)
        if top:
            g.desc_cache[(a["r"], 8)] = idx
        return idx

    g.scalar_load, g.desc_lookup_sgpr = scalar_load, desc_lookup_sgpr


def slot_lookup(g, addr, words):
    t = g.tmp

    def word(i):
        p, v = t(), t()
        g.emit(f"{p} = OpAccessChain %_ptr_SB_uint %desc_map %c0 {i}")
        g.emit(f"{v} = OpLoad %uint {p}")
        return v
    mask = word("%c0")
    m1, m10, h = t(), t(), t()
    g.emit(f"{m1} = OpIAdd %uint {mask} {g.cu(1)}")
    g.emit(f"{m10} = OpIMul %uint {m1} {g.cu(10)}")
    g.emit(f"{h} = OpIAdd %uint {m10} {g.cu(1)}")
    hw = []
    for k in range(5):
        i = t()
        g.emit(f"{i} = OpIAdd %uint {h} {g.cu(k)}")
        hw.append(word(i))
    magic, va_lo, va_hi, nslots, toff = hw
    vl, vh, vs, va, rel, n64, lim, inr, low, al, okm, ok1, ok = (t() for _ in range(13))
    g.emit(f"{vl} = OpUConvert %ulong {va_lo}")
    g.emit(f"{vh} = OpUConvert %ulong {va_hi}")
    g.emit(f"{vs} = OpShiftLeftLogical %ulong {vh} %c64_32")
    g.emit(f"{va} = OpBitwiseOr %ulong {vs} {vl}")
    g.emit(f"{rel} = OpISub %ulong {addr} {va}")
    g.emit(f"{n64} = OpUConvert %ulong {nslots}")
    g.emit(f"{lim} = OpShiftLeftLogical %ulong {n64} {g.cl(5)}")
    g.emit(f"{inr} = OpULessThan %bool {rel} {lim}")
    g.emit(f"{low} = OpBitwiseAnd %ulong {rel} {g.cl(31)}")
    g.emit(f"{al} = OpIEqual %bool {low} {g.cl(0)}")
    g.emit(f"{okm} = OpIEqual %bool {magic} {g.cu(HEAP_MAGIC)}")
    g.emit(f"{ok1} = OpLogicalAnd %bool {okm} {inr}")
    g.emit(f"{ok} = OpLogicalAnd %bool {ok1} {al}")
    pre = g.current_label_placeholder()
    l_in, l_in_end = t() + "_tix", t() + "_tixe"
    g.emit(f"OpSelectionMerge {l_in_end} None")
    g.emit(f"OpBranchConditional {ok} {l_in} {l_in_end}")
    g.emit(f"{l_in} = OpLabel")
    s64, s32, s9, e = t(), t(), t(), t()
    g.emit(f"{s64} = OpShiftRightLogical %ulong {rel} {g.cl(5)}")
    g.emit(f"{s32} = OpUConvert %uint {s64}")
    g.emit(f"{s9} = OpIMul %uint {s32} {g.cu(9)}")
    g.emit(f"{e} = OpIAdd %uint {toff} {s9}")
    match = "%true"
    for j, w in enumerate(words):
        i, eq, m2 = t(), t(), t()
        g.emit(f"{i} = OpIAdd %uint {e} {g.cu(j)}")
        v = word(i)
        g.emit(f"{eq} = OpIEqual %bool {v} {w}")
        g.emit(f"{m2} = OpLogicalAnd %bool {match} {eq}")
        match = m2
    i8 = t()
    g.emit(f"{i8} = OpIAdd %uint {e} {g.cu(8)}")
    sidx = word(i8)
    nz, hit_in = t(), t()
    g.emit(f"{nz} = OpINotEqual %bool {sidx} {g.cu(0)}")
    g.emit(f"{hit_in} = OpLogicalAnd %bool {match} {nz}")
    l_in_last = g.current_label_placeholder()
    g.emit(f"OpBranch {l_in_end}")
    g.emit(f"{l_in_end} = OpLabel")
    hit, hidx = t(), t()
    g.emit(f"{hit} = OpPhi %bool {hit_in} {l_in_last} %false {pre}")
    g.emit(f"{hidx} = OpPhi %uint {sidx} {l_in_last} {g.cu(0)} {pre}")
    l_miss, l_end = t() + "_tixm", t() + "_tixd"
    pre2 = g.current_label_placeholder()
    g.emit(f"OpSelectionMerge {l_end} None")
    g.emit(f"OpBranchConditional {hit} {l_end} {l_miss}")
    g.emit(f"{l_miss} = OpLabel")
    midx = g.desc_lookup(words)
    l_miss_last = g.current_label_placeholder()
    g.emit(f"OpBranch {l_end}")
    g.emit(f"{l_end} = OpLabel")
    r = t()
    g.emit(f"{r} = OpPhi %uint {hidx} {pre2} {midx} {l_miss_last}")
    return r
