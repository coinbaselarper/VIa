"""Lane-uniform votes and readfirstlanes under --wave64-emulate (translator-fork addition, not in leonida d3b8251b).

Under --wave64-emulate every cross-lane operation is an exchange round: the lanes publish their share in workgroup
memory, the workgroup meets at a barrier, and the next segment reads the wave's answer. Many of them ask about
values that are the same in every lane of the wave by construction: an EXEC vote after a compare of scalars and
constants, the SCC of s_and_saveexec_b64 on such a compare, a v_readfirstlane_b32 of a VGPR written from scalars.
In a wave whose 64 lanes all exist (the workgroup size is a multiple of 64), the answer then is the lane's own:
the OR of 64 equal bits is the bit, the first lane's copy of an equal value is the value. This module proves which.

    analyse(blocks, succ, lane_masks) -> {"votes": {id(ins)}, "rfl": {id(ins)}}

votes: instructions (and branch terminators) all of whose votes are over lane-uniform masks; the translator then
computes them from the lane's own bit with no exchange. rfl: v_readfirstlane_b32 / v_readlane_b32 of a VGPR that
holds one value in all 64 lanes; the translator reads the lane's own copy.

A forward dataflow over the blocks, meet = intersection, with four facts per program point:
  uv    VGPRs that hold one value in all 64 lanes (none at entry: no assumption about input or unwritten VGPRs)
  ns    scalars (SGPRs, vcc as data, scc, m0) that may differ by lane: only the lane-local copies a folded waterfall
        loop (waterfall_fold.py) makes, and what is computed from them; every other scalar is one value per wave,
        as on hardware
  lu    lane masks (exec, vcc, SGPRs holding a mask per lane_masks.py) whose bit is the same in all 64 lanes
  full  lane masks known to have every bit set (exec at entry)
A VALU write is uniform when its sources are and EXEC is full (or EXEC is lane-uniform and the old value was
uniform too: then the write happens in all lanes or in none). Loads, LDS, lane ids and other cross-lane results are
never uniform. Anything not modelled makes its destinations non-uniform.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
import lane_masks as LM  # noqa: E402

BRANCHES = {"s_branch", "s_cbranch_scc0", "s_cbranch_scc1", "s_cbranch_vccz", "s_cbranch_vccnz",
            "s_cbranch_execz", "s_cbranch_execnz"}
MASK_BITWISE = ("s_and_b", "s_or_b", "s_xor_b", "s_andn2_b", "s_orn2_b", "s_nand_b", "s_nor_b", "s_xnor_b")
MASK_COPIES = ("s_mov_b", "s_not_b", "s_wqm_b")
CARRY_IN = ("v_cndmask", "v_addc", "v_subb", "v_add_co_ci", "v_sub_co_ci", "v_subrev_co_ci")
TWO_DEST = ("v_add_co_u32", "v_sub_co_u32", "v_subrev_co_u32", "v_add_co_ci_u32", "v_sub_co_ci_u32",
            "v_subrev_co_ci_u32", "v_mad_u64_u32", "v_mad_i64_i32", "v_div_scale_f32", "v_div_scale_f64")
NOT_UNIFORM_VALU = ("v_mbcnt", "v_readlane", "v_readfirstlane", "v_writelane", "v_permlane", "v_movrel", "v_swap",
                    "v_interp", "v_mov_b32_dpp", "v_div_fmas", "v_div_fixup_f64")
LOADS = ("buffer_", "tbuffer_", "global_", "flat_", "scratch_", "image_", "ds_")
NO_DEST = ("s_cmp", "s_bitcmp", "v_cmpx", "s_set", "s_store", "s_buffer_store", "s_dcache", "s_icache", "s_sendmsg",
           "s_branch", "s_cbranch", "s_waitcnt", "s_nop", "s_sleep", "s_barrier", "s_endpgm", "exp", "buffer_store",
           "tbuffer_store", "global_store", "flat_store", "scratch_store", "image_store", "ds_write", "s_inst_prefetch",
           "s_clause", "s_trap", "v_nop", "s_setprio", "s_code_end", "s_denorm_mode", "s_round_mode", "s_ttracedata",
           "buffer_gl", "s_waitcnt_vscnt")


def base_op(op):
    for suf in ("_e32", "_e64", "_sdwa", "_dpp"):
        if op.endswith(suf):
            return op[: -len(suf)]
    return op


class State:
    __slots__ = ("uv", "ns", "lu", "full")

    def __init__(self, uv=frozenset(), ns=frozenset(), lu=frozenset(), full=frozenset()):
        self.uv, self.ns, self.lu, self.full = set(uv), set(ns), set(lu), set(full)

    def copy(self):
        return State(self.uv, self.ns, self.lu, self.full)

    def key(self):
        return (frozenset(self.uv), frozenset(self.ns), frozenset(self.lu), frozenset(self.full))

    def meet(self, o):
        return State(self.uv & o.uv, self.ns | o.ns, self.lu & o.lu, self.full & o.full)


def _sregs(x):
    k = x.get("k")
    if k == "sgpr":
        return list(range(x["r"], x["r"] + x.get("n", 1)))
    if k == "special":
        t = x.get("t", "")
        if t.startswith("vcc"):
            return ["vcc"]
        if t.startswith("exec"):
            return ["exec"]
        if t in ("scc", "src_scc"):
            return ["scc"]
        if t == "m0":
            return ["m0"]
    if k == "slist":
        return [r if isinstance(r, int) else "vcc" for r in x.get("r", ())]
    return []


def _vregs(x):
    if x.get("k") == "vgpr":
        return list(range(x["r"], x["r"] + x.get("n", 1)))
    if x.get("k") == "vlist":
        return [r for r in x.get("r", ()) if isinstance(r, int)]
    return []


class Analysis:
    def __init__(self, lane_masks, either):
        self.lm = lane_masks
        self.either = either
        self.votes = set()
        self.rfl = set()

    def is_mask(self, ins, r):
        m = self.lm.get(id(ins), {})
        if r == "exec":
            return True
        return r in m

    def mask_lu(self, st, ins, x):
        """Whether operand x, read as a lane mask, has the same bit in all 64 lanes."""
        k = x.get("k")
        if k == "imm":
            return (x["v"] & 0xFFFFFFFF) in (0, 0xFFFFFFFF)
        if k == "special" and x.get("t") == "null":
            return True
        regs = _sregs(x)
        if not regs or "scc" in regs or "m0" in regs:
            return False
        ev = self.either.get(id(ins), ())
        for r in regs:
            if r == "exec" or self.is_mask(ins, r):
                if r not in st.lu:
                    return False
            elif r not in ev:
                return False
        return True

    def mask_full(self, st, ins, x):
        k = x.get("k")
        if k == "imm":
            return (x["v"] & 0xFFFFFFFF) == 0xFFFFFFFF
        regs = _sregs(x)
        return bool(regs) and all((r == "exec" or self.is_mask(ins, r)) and r in st.full for r in regs)

    def val_u(self, st, ins, x):
        """Whether operand x, read as a value, is the same in all 64 lanes."""
        k = x.get("k")
        if k in ("imm", "fimm", "lit", "literal", "const"):
            return True
        if k == "vgpr" or k == "vlist":
            return all(r in st.uv for r in _vregs(x))
        if k == "special":
            t = x.get("t", "")
            if t == "null":
                return True
            if t.startswith(("exec", "vcc")):
                r = "exec" if t.startswith("exec") else "vcc"
                if r == "exec" or self.is_mask(ins, r):
                    return r in st.lu
                return r not in st.ns
            if t in ("scc", "m0"):
                return t not in st.ns
            return False
        if k in ("sgpr", "slist"):
            for r in _sregs(x):
                if self.is_mask(ins, r):
                    if r not in st.lu:
                        return False
                elif r in st.ns:
                    return False
            return True
        return False

    def set_scalar(self, st, regs, uniform):
        for r in regs:
            if r == "exec":
                continue
            st.lu.discard(r)
            st.full.discard(r)
            if uniform:
                st.ns.discard(r)
            else:
                st.ns.add(r)

    def set_mask(self, st, regs, lu, full):
        for r in regs:
            st.ns.discard(r)
            if lu:
                st.lu.add(r)
            else:
                st.lu.discard(r)
            if full:
                st.full.add(r)
            else:
                st.full.discard(r)

    def set_vgprs(self, st, regs, uniform):
        exec_full = "exec" in st.full
        exec_lu = "exec" in st.lu
        for r in regs:
            if uniform and (exec_full or (exec_lu and r in st.uv)):
                st.uv.add(r)
            else:
                st.uv.discard(r)

    def step(self, st, ins):
        op = base_op(ins["op"])
        raw = ins["op"]
        a = ins["args"]
        mods = ins.get("mods", ())
        if op in BRANCHES:
            if op in ("s_cbranch_execz", "s_cbranch_execnz"):
                if "exec" in st.lu:
                    self.votes.add(id(ins))
            elif op in ("s_cbranch_vccz", "s_cbranch_vccnz"):
                if self.is_mask(ins, "vcc"):
                    if "vcc" in st.lu:
                        self.votes.add(id(ins))
                elif "vcc" not in st.ns:
                    self.votes.add(id(ins))
            return
        if op == "s_endpgm" or not a:
            return
        if op.startswith(("v_cmp_", "v_cmpx_")) or op.startswith(("v_cmp", "v_cmpx")):
            cmpx = op.startswith("v_cmpx")
            srcs = a if cmpx else (a[1:] if len(a) >= 3 else a)
            u = all(self.val_u(st, ins, x) for x in srcs) and "exec" in st.lu and not raw.endswith("_dpp")
            if cmpx:
                if not u:
                    st.lu.discard("exec")
                st.full.discard("exec")
                return
            dst = _sregs(a[0]) if len(a) >= 3 else ["vcc"]
            self.set_mask(st, dst, u, False)
            return
        if op == "v_readfirstlane_b32" or op == "v_readlane_b32":
            src = a[1]
            lm = self.lm.get(id(ins), {})
            plain = src.get("k") == "vgpr" and ("v", src["r"]) not in lm
            if op == "v_readlane_b32" and len(a) > 2:
                slot = LM.lane_slot(a[1], a[2])
                plain = plain and slot not in lm
            su = plain and self.val_u(st, ins, src)
            if su:
                self.rfl.add(id(ins))
            dst = _sregs(a[0])
            if dst and self.is_mask(ins, dst[0]):
                self.set_mask(st, dst, False, False)
            elif ins.get("own_lane"):
                self.set_scalar(st, dst, su)
            else:
                self.set_scalar(st, dst, True)
            return
        if op.startswith("s_") and ("_saveexec_b" in op or "_wrexec_b" in op):
            src = a[1]
            src_lu = self.mask_lu(st, ins, src)
            exec_lu = "exec" in st.lu
            exec_full = "exec" in st.full
            if src_lu and exec_lu:
                self.votes.add(id(ins))
            d = _sregs(a[0])
            if "_saveexec_b" in op:
                kind = op.split("_")[1]
                self.set_mask(st, d, exec_lu, exec_full)
                new_lu = exec_lu and src_lu
                new_full = (exec_full and self.mask_full(st, ins, src)) if kind == "and" else \
                    (exec_full or self.mask_full(st, ins, src)) if kind == "or" else False
            else:
                new_lu = exec_lu and src_lu
                new_full = False
                self.set_mask(st, d, new_lu, False)
            (st.lu.add if new_lu else st.lu.discard)("exec")
            (st.full.add if new_full else st.full.discard)("exec")
            st.ns.discard("scc")
            return
        if op.startswith("s_") and (op.startswith(MASK_BITWISE) or op.startswith(MASK_COPIES) or op.startswith("s_cselect_b")):
            dst = _sregs(a[0])
            srcs = a[1:]
            is_mask_op = (dst and (dst[0] == "exec" or self.is_mask_after(ins, dst[0]))) or \
                any(self.is_mask(ins, r) for x in srcs for r in _sregs(x))
            if is_mask_op:
                lu = all(self.mask_lu(st, ins, x) for x in srcs)
                if op.startswith("s_cselect_b"):
                    lu = lu and "scc" not in st.ns
                if lu and "exec" in st.lu:
                    self.votes.add(id(ins))
                if op.startswith("s_mov_b"):
                    full = self.mask_full(st, ins, srcs[0])
                elif op.startswith("s_or_b"):
                    full = any(self.mask_full(st, ins, x) for x in srcs)
                elif op.startswith("s_and_b"):
                    full = all(self.mask_full(st, ins, x) for x in srcs)
                else:
                    full = False
                if op.endswith("_b32"):
                    full = False
                half = a[0].get("k") == "special" and a[0].get("t") in ("exec_lo", "exec_hi", "vcc_lo", "vcc_hi")
                if half:
                    lu = lu and dst[0] in st.lu
                self.set_mask(st, dst if dst != ["exec"] else [], lu, full)
                if dst == ["exec"] or (a[0].get("k") == "special" and a[0].get("t", "").startswith("exec")):
                    (st.lu.add if lu else st.lu.discard)("exec")
                    (st.full.add if full and a[0].get("t") == "exec" else st.full.discard)("exec")
                if not op.startswith("s_mov_b") and not op.startswith("s_cselect_b"):
                    st.ns.discard("scc")
                return
            u = all(self.val_u(st, ins, x) for x in srcs)
            self.set_scalar(st, dst, u)
            if not op.startswith(("s_mov_b", "s_cselect_b")):
                self.set_scalar(st, ["scc"], u)
            return
        if op.startswith(("s_load", "s_buffer_load")):
            u = all(self.val_u(st, ins, x) for x in a[1:]) and not ins.get("exec_guard")
            self.set_scalar(st, _sregs(a[0]), u)
            return
        if op.startswith(("s_movrel", "s_getpc", "s_setpc", "s_swappc", "s_memtime", "s_memrealtime", "s_getreg",
                          "s_bcnt", "s_ff1", "s_ff0", "s_flbit", "s_quadmask", "s_bitreplicate")):
            u = op.startswith(("s_getpc", "s_memtime", "s_memrealtime", "s_getreg")) or \
                (op.startswith("s_movrel") and not st.ns)
            if op.startswith(("s_bcnt", "s_ff1", "s_ff0", "s_flbit", "s_quadmask")):
                src = a[1] if len(a) > 1 else None
                if src is not None and any(self.is_mask(ins, r) for r in _sregs(src)):
                    if self.mask_lu(st, ins, src) and "exec" in st.lu:
                        self.votes.add(id(ins))
                    u = True
                else:
                    u = src is not None and self.val_u(st, ins, src)
            dst = _sregs(a[0])
            if op.startswith("s_movrel") and not u:
                for r in range(128):
                    st.ns.add(r)
            self.set_scalar(st, dst, u)
            self.set_scalar(st, ["scc"], u)
            return
        if op.startswith("s_"):
            if op.startswith(NO_DEST):
                if op.startswith(("s_cmp", "s_bitcmp")):
                    u = all(self.val_u(st, ins, x) for x in a)
                    if any(self.is_mask(ins, r) for x in a for r in _sregs(x)):
                        lu = all(self.mask_lu(st, ins, x) for x in a)
                        if lu and "exec" in st.lu:
                            self.votes.add(id(ins))
                        u = True
                    self.set_scalar(st, ["scc"], u)
                return
            dst = _sregs(a[0])
            u = all(self.val_u(st, ins, x) for x in a[1:])
            if dst and dst[0] == "exec":
                st.lu.discard("exec")
                st.full.discard("exec")
                return
            self.set_scalar(st, dst, u)
            self.set_scalar(st, ["scc"], u)
            return
        if op.startswith(LOADS):
            for x in a[:1]:
                self.set_vgprs(st, _vregs(x), False)
            if op.startswith(("buffer_atomic", "global_atomic", "flat_atomic", "image_atomic", "ds_")):
                for x in a[:1]:
                    for r in _vregs(x):
                        st.uv.discard(r)
            return
        if op.startswith("v_"):
            if op.startswith(NOT_UNIFORM_VALU) or raw.endswith("_dpp") or any(
                    m.startswith(("quad_perm", "row_", "wave_", "dpp8")) for m in mods):
                for x in a[:2]:
                    self.set_vgprs(st, _vregs(x), False)
                    if x.get("k") in ("sgpr", "special"):
                        self.set_mask(st, _sregs(x), False, False)
                if op.startswith(("v_movrel", "v_swap")):
                    st.uv.clear()
                return
            two = op in TWO_DEST and len(a) > 2 and a[1].get("k") in ("sgpr", "special", "slist")
            srcs = a[2:] if two else a[1:]
            if op.startswith(CARRY_IN):
                *vals, m = srcs if srcs else [None]
                u = all(self.val_u(st, ins, x) for x in vals) and m is not None and self.mask_lu(st, ins, m)
            else:
                u = all(self.val_u(st, ins, x) for x in srcs)
            if op.startswith("v_div_fmas"):
                u = u and self.mask_lu(st, ins, {"k": "special", "t": "vcc"})
            if a[0].get("k") == "vgpr":
                self.set_vgprs(st, _vregs(a[0]), u)
            elif a[0].get("k") in ("sgpr", "special"):
                self.set_scalar(st, _sregs(a[0]), False)
            if two:
                self.set_mask(st, _sregs(a[1]), u and "exec" in st.lu, False)
            return
        if op == "exp":
            return
        for x in a[:1]:
            self.set_vgprs(st, _vregs(x), False)
            self.set_scalar(st, [r for r in _sregs(x) if r != "exec"], False)

    def is_mask_after(self, ins, r):
        return r == "exec" or r in self._after.get(id(ins), ())


def analyse(blocks, succ, lane_masks, wave64=True):
    _, _, either = LM.analyse(blocks, succ, include_either=True, wave64=wave64)
    an = Analysis(lane_masks, either)
    an._after = {}
    for blk in blocks:
        for k, ins in enumerate(blk):
            nxt = blk[k + 1] if k + 1 < len(blk) else None
            an._after[id(ins)] = set(lane_masks.get(id(nxt), {})) if nxt is not None else set()
    n = len(blocks)
    entry = [None] * n
    if n:
        entry[0] = State(lu={"exec"} | set(range(128)) | {"vcc"}, full={"exec"})
    work = [0] if n else []
    seen = set()
    while work:
        b = work.pop()
        st = entry[b].copy()
        for ins in blocks[b]:
            an.step(st, ins)
        for s in succ[b]:
            m = st if entry[s] is None else entry[s].meet(st)
            if entry[s] is None or m.key() != entry[s].key():
                entry[s] = m
                work.append(s)
        seen.add(b)
    an.votes.clear()
    an.rfl.clear()
    for b in range(n):
        if entry[b] is None:
            continue
        st = entry[b].copy()
        for ins in blocks[b]:
            an.step(st, ins)
    return {"votes": an.votes, "rfl": an.rfl}
