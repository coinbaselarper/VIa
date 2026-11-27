"""Waterfall loops under --wave64-emulate (translator-fork addition, not in leonida d3b8251b).

The console compiler lowers a non-uniform descriptor (NonUniformResourceIndex) to a waterfall loop:

    L:  v_readfirstlane_b32 sA_i, vB_i           (i = 1..n)
        v_cmp_eq_u32/u64 M_j, sA.., vB..         (every sA_i compared with its own vB_i)
        s_and_b64 ...                            (the compares ANDed into M)
        s_and_saveexec_b64 sS, M
        BODY                                     (loads/samples through sA, lane-local arithmetic)
        s_xor_b64 exec, exec, sS
        s_cbranch_execnz L
        s_mov_b64 exec, sX

Each lane that is on at L is on in exactly one iteration, the one whose first lane holds the same sA words as this
lane's vB words, and its BODY runs once with sA equal to its own vB. In the translation one invocation is one lane
with its own copy of every register, so that iteration is the lane's own copy: sA_i := vB_i of this lane, BODY once
under the EXEC of L, no compare, no vote. Under --wave64-emulate the readfirstlanes and the vote are one exchange
round each; the folded loop has none.

fold(insns, next_pc, target) -> (insns, report). The rewrite is applied only where it is exact:
- the loop is one straight block (no branch into it but the back edge, no branch inside);
- the head holds only readfirstlanes, equality compares, s_and_b64 of compare masks, s_nop, s_waitcnt, and M is the
  AND of compares that cover every readfirstlane destination with exactly its source VGPR;
- BODY: scalar loads (one addressed through sA loads only in lanes that are on: an inactive lane holds a stale sA),
  lane-local VALU without scalar results, buffer/global loads, explicit-LOD image reads, scalar ALU; whatever
  depends on sA only into registers dead at exit or overwritten by loop-invariant code; no store, atomic, LDS,
  export, barrier, cross-lane op or EXEC/VCC/SCC read; no body write to a vB; no scalar read of a register the loop
  writes later (loop-carried);
- after the loop EXEC is set by s_mov_b64 from a register the loop does not write before anything reads it, and
  every scalar the loop writes from per-lane values (sA, the masks, sS, VCC, SCC) is dead there.
"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
import wave64_emulate_opt as W  # noqa: E402

BRANCH_OPS = W.BRANCHES | {"s_setpc_b64", "s_swappc_b64", "s_endpgm"}
VCC = W._BIT_VCC
SCC = W._BIT_SCC
NOT_LANE_LOCAL = ("v_cmp", "v_readlane", "v_readfirstlane", "v_writelane", "v_permlane", "v_mbcnt", "v_interp",
                  "v_div_scale", "v_mad_u64_u32", "v_mad_i64_i32", "v_add_co", "v_sub_co", "v_subrev_co",
                  "v_cndmask", "v_addc", "v_subb", "v_subbrev", "v_div_fmas", "v_movrel", "v_swap")
SALU_OK = frozenset({"s_mov_b32", "s_movk_i32", "s_and_b32", "s_or_b32", "s_xor_b32", "s_andn2_b32", "s_orn2_b32",
                     "s_not_b32", "s_lshl_b32", "s_lshr_b32", "s_ashr_i32", "s_add_u32", "s_sub_u32", "s_add_i32",
                     "s_sub_i32", "s_mul_i32", "s_bfe_u32", "s_bfe_i32", "s_min_u32", "s_max_u32", "s_min_i32",
                     "s_max_i32", "s_addk_i32", "s_mulk_i32", "s_lshl1_add_u32", "s_lshl2_add_u32",
                     "s_lshl3_add_u32", "s_lshl4_add_u32", "s_bfm_b32", "s_mov_b64"})


def base_op(op):
    for suf in ("_e32", "_e64", "_sdwa", "_dpp"):
        if op.endswith(suf):
            return op[: -len(suf)]
    return op


def _sgprs(x):
    if x.get("k") == "sgpr":
        return list(range(x["r"], x["r"] + x.get("n", 1)))
    return []


def _is_special(x, *names):
    return x.get("k") == "special" and x.get("t", "") in names


def _mask_key(x):
    if _is_special(x, "vcc"):
        return ("vcc",)
    if x.get("k") == "sgpr" and x.get("n", 1) == 2:
        return ("s", x["r"])
    if _is_special(x, "exec"):
        return ("exec",)
    return None


def _vgprs(x):
    if x.get("k") == "vgpr":
        return list(range(x["r"], x["r"] + x.get("n", 1)))
    if x.get("k") == "vlist":
        return [r for r in x["r"] if isinstance(r, int)]
    return []


def _mask_bits(key):
    if key == ("vcc",):
        return 1 << VCC
    if key and key[0] == "s":
        return (1 << key[1]) | (1 << (key[1] + 1))
    return 0


def _explicit_image(op):
    segs = op.split("_")
    if len(segs) < 2 or segs[0] != "image":
        return False
    if segs[1] in ("load", "get"):
        return not any(s in ("store", "atomic") for s in segs)
    if segs[1] in ("sample", "gather4"):
        return any(s in ("lz", "l", "d") for s in segs[2:]) and "b" not in segs[2:] and "cd" not in segs[2:]
    return False


def _body_kind(ins):
    """'smem', 'salu', 'valu', 'vmem', 'nop' or None (not allowed)."""
    op = base_op(ins["op"])
    a = ins["args"]
    if op in ("s_nop", "s_waitcnt") or op.startswith("s_waitcnt"):
        return "nop"
    if op.startswith(("s_load_", "s_buffer_load_")):
        return "smem"
    if op.startswith(("buffer_load_", "global_load_", "tbuffer_load_")):
        if any(m in ins.get("mods", ()) for m in ("lds", "tfe")):
            return None
        return "vmem"
    if op.startswith("image_"):
        return "vmem" if _explicit_image(op) else None
    if op.startswith("v_"):
        if op.startswith(NOT_LANE_LOCAL) or ins["op"].endswith("_dpp"):
            return None
        if any(m.startswith(("quad_perm", "row_", "wave_", "dpp8")) for m in ins.get("mods", ())):
            return None
        if any(x.get("k") in ("sgpr", "special") and not _is_special(x, "m0") for x in a[:1]):
            return None
        return "valu"
    if op in SALU_OK:
        if not a or a[0].get("k") != "sgpr":
            return None
        return "salu"
    return None


def _reads_special(ins, start, names):
    return any(_is_special(x, *names) for x in ins["args"][start:])


def _blocks(insns, next_pc, target):
    leaders = {insns[0]["pc"]}
    for ins in insns:
        if base_op(ins["op"]) in W.BRANCHES:
            leaders.add(target(ins))
            leaders.add(next_pc(ins))
        elif base_op(ins["op"]) == "s_endpgm":
            leaders.add(next_pc(ins))
    pcs = {i["pc"] for i in insns}
    starts = sorted(p for p in leaders if p in pcs)
    index = {p: n for n, p in enumerate(starts)}
    blocks = [[] for _ in starts]
    cur = -1
    for ins in insns:
        if ins["pc"] in index:
            cur = index[ins["pc"]]
        blocks[cur].append(ins)
    succ = []
    for n, blk in enumerate(blocks):
        last = next((i for i in blk if base_op(i["op"]) == "s_endpgm" or base_op(i["op"]) in W.BRANCHES), None)
        fall = [n + 1] if n + 1 < len(blocks) else []
        if last is None:
            succ.append(fall)
        elif base_op(last["op"]) == "s_endpgm":
            succ.append([])
        elif base_op(last["op"]) == "s_branch":
            succ.append([index[target(last)]] if target(last) in index else [])
        else:
            succ.append(([index[target(last)]] if target(last) in index else []) + fall)
    return blocks, succ, index


def _bits(regs):
    m = 0
    for r in regs:
        m |= 1 << r
    return m


def _try(insns, i, j, live_at_exit, restore_bits, targeted=None):
    """Check insns[i..j] (j = the s_cbranch_execnz) as a waterfall loop. Returns (plan, None) or (None, reason).
    Two forms: compares + s_and_saveexec_b64 sS ... s_xor_b64 exec, exec, sS; or v_cmpx compares ...
    s_andn2_wrexec_b64 sR, sR with sR = exec at the loop head."""
    loop = insns[i:j + 1]
    k = 0
    rfl = {}
    cov = {}
    cmpx_cov = set()
    form = None
    s_s = None
    while k < len(loop) - 1:
        ins = loop[k]
        op = base_op(ins["op"])
        a = ins["args"]
        if op == "v_readfirstlane_b32":
            if a[0].get("k") != "sgpr" or a[0].get("n", 1) != 1 or a[1].get("k") != "vgpr":
                return None, "rfl-operands"
            s, v = a[0]["r"], a[1]["r"]
            if s in rfl:
                return None, "rfl-twice"
            rfl[s] = v
        elif op in ("v_cmp_eq_u32", "v_cmp_eq_u64", "v_cmpx_eq_u32", "v_cmpx_eq_u64"):
            cmpx = op.startswith("v_cmpx")
            if form is not None and form != ("w" if cmpx else "x"):
                return None, "cmp-mixed"
            srcs = a[1:] if not cmpx and len(a) == 3 else a if cmpx and len(a) == 2 else None
            if srcs is None:
                return None, "cmp-form"
            if any(m for m in ins.get("mods", ()) if m.startswith(("src0_sel", "src1_sel")) and not m.endswith("DWORD")):
                return None, "cmp-sdwa"
            sx = next((x for x in srcs if x.get("k") == "sgpr"), None)
            vx = next((x for x in srcs if x.get("k") == "vgpr"), None)
            if sx is None or vx is None or sx.get("neg") or sx.get("abs") or vx.get("neg") or vx.get("abs"):
                return None, "cmp-operands"
            width = 2 if op.endswith("u64") else 1
            if sx.get("n", 1) != width or vx.get("n", 1) != width:
                return None, "cmp-width"
            pairs = [(sx["r"] + t, vx["r"] + t) for t in range(width)]
            if any(rfl.get(s) != v for s, v in pairs):
                return None, "cmp-not-rfl"
            if cmpx:
                form = "w"
                cmpx_cov.update(s for s, _ in pairs)
            else:
                form = "x"
                dst = _mask_key(a[0])
                if dst is None or dst == ("exec",):
                    return None, "cmp-dst"
                cov[dst] = frozenset(s for s, _ in pairs)
        elif op == "s_and_b64" and form == "x":
            d, x, y = (_mask_key(t) for t in a[:3])
            if d is None or x is None or y is None or d == ("exec",):
                return None, "and-operands"
            cx = frozenset() if x == ("exec",) else cov.get(x)
            cy = frozenset() if y == ("exec",) else cov.get(y)
            if cx is None or cy is None:
                return None, "and-not-cmp"
            cov[d] = cx | cy
        elif op == "s_and_saveexec_b64" and form == "x":
            d, m = _mask_key(a[0]), _mask_key(a[1])
            if d is None or m is None or d[0] != "s" or cov.get(m) is None:
                return None, "saveexec-operands"
            if not rfl or not set(rfl) <= cov[m]:
                return None, "not-all-compared"
            s_s = d
            k += 1
            break
        elif op == "s_nop" or op.startswith("s_waitcnt"):
            pass
        elif form == "w" and rfl and set(rfl) <= cmpx_cov:
            break
        else:
            return None, "head-op:" + op
        k += 1
    if form is None or (form == "x" and s_s is None) or (form == "w" and not (rfl and set(rfl) <= cmpx_cov)):
        return None, "no-mask"
    tail = len(loop) - 2
    while tail > k and base_op(loop[tail]["op"]) == "s_nop":
        tail -= 1
    xo = loop[tail]
    xa = [_mask_key(t) for t in xo["args"]]
    if form == "x":
        if base_op(xo["op"]) != "s_xor_b64" or len(xa) != 3 or xa[0] != ("exec",) \
                or sorted(map(str, xa[1:])) != sorted(map(str, [("exec",), s_s])):
            return None, "no-xor"
        mask_reg = s_s
    else:
        if base_op(xo["op"]) != "s_andn2_wrexec_b64" or len(xa) != 2 or xa[0] != xa[1] or not xa[0] or xa[0][0] != "s":
            return None, "no-wrexec"
        mask_reg = xa[0]
        pre = i - 1
        while pre >= 0 and (base_op(insns[pre]["op"]) == "s_nop" or base_op(insns[pre]["op"]).startswith("s_waitcnt")
                            or not (W._scalar_rw(insns[pre])[1] & _mask_bits(mask_reg)) and not _writes_exec(insns[pre])):
            if base_op(insns[pre]["op"]) in BRANCH_OPS:
                return None, "wrexec-pre-branch"
            pre -= 1
        if pre < 0:
            return None, "wrexec-no-pre"
        pa = insns[pre]["args"]
        if not (base_op(insns[pre]["op"]) == "s_mov_b64" and len(pa) == 2 and _mask_key(pa[0]) == mask_reg
                and _is_special(pa[1], "exec")):
            return None, "wrexec-pre"
        if targeted is not None and any(targeted.get(x["pc"], 0) for x in insns[pre + 1:i]):
            return None, "wrexec-pre-target"
    body = loop[k:tail]
    dropped = set(q for q in range(k) if base_op(loop[q]["op"]) != "v_readfirstlane_b32"
                  and base_op(loop[q]["op"]) != "s_nop" and not base_op(loop[q]["op"]).startswith("s_waitcnt"))
    dropped.update(range(tail, len(loop)))
    rfl_bits = _bits(rfl)
    vb = set(rfl.values())
    head_mask_bits = 0
    for q in range(k):
        if q in dropped:
            head_mask_bits |= W._scalar_rw(loop[q])[1]
    head_mask_bits |= _mask_bits(mask_reg)
    writes = collections.defaultdict(list)
    for p, ins in enumerate(loop):
        mw = W._scalar_rw(ins)[1]
        b = mw
        while b:
            low = b & -b
            writes[low.bit_length() - 1].append(p)
            b ^= low
    for p, ins in enumerate(loop):
        if p >= tail:
            continue
        rd = W._scalar_rw(ins)[0]
        b = rd
        while b:
            low = b & -b
            r = low.bit_length() - 1
            b ^= low
            ws = writes.get(r, ())
            if any(q >= p for q in ws) and not any(q < p for q in ws):
                return None, "loop-carried"
    taint = rfl_bits
    stale = head_mask_bits
    last = {}
    guard = []
    for q in range(len(loop)):
        mw = W._scalar_rw(loop[q])[1]
        b = mw
        while b:
            low = b & -b
            last[low.bit_length() - 1] = ("drop" if q in dropped else "rfl" if q < k else None, q)
            b ^= low
    for q, ins in enumerate(body, start=k):
        kind = _body_kind(ins)
        if kind is None:
            return None, "body-op:" + base_op(ins["op"])
        if kind == "nop":
            continue
        a = ins["args"]
        if any(_is_special(x, "exec", "exec_lo", "exec_hi", "scc", "src_scc", "vcc", "vcc_lo", "vcc_hi") for x in a):
            return None, "body-reads-mask"
        rd, mw, mk = W._scalar_rw(ins)
        if rd & stale:
            return None, "body-reads-head-mask"
        stale &= ~mk
        if mw & _mask_bits(mask_reg):
            return None, "body-writes-mask-reg"
        tainted = bool(rd & taint)
        if kind == "smem" and tainted:
            guard.append(i + q)
        if kind in ("smem", "salu"):
            if tainted:
                taint |= mw
            else:
                taint &= ~mk
            b = mw
            while b:
                low = b & -b
                r = low.bit_length() - 1
                if last.get(r, (None, -1))[1] == q:
                    last[r] = ("taint" if tainted else "inv", q)
                b ^= low
        if set().union(*[_vgprs(x) for x in a[:1]]) & vb:
            return None, "body-writes-vb"
    must_die = 1 << SCC
    for r, (why, q) in last.items():
        if why in ("drop", "rfl", "taint"):
            must_die |= 1 << r
    if live_at_exit & must_die:
        return None, "live-at-exit"
    if restore_bits is None:
        return None, "no-exec-restore"
    loop_writes = 0
    for ins in loop:
        loop_writes |= W._scalar_rw(ins)[1]
    if restore_bits & loop_writes:
        return None, "restore-from-loop-reg"
    return {"rfl": rfl, "form": form, "drop": [i + q for q in sorted(dropped)],
            "own": [i + q for q in range(k) if base_op(loop[q]["op"]) == "v_readfirstlane_b32"],
            "guard": guard}, None


def _writes_exec(ins):
    op = base_op(ins["op"])
    a = ins["args"]
    if op.startswith("v_cmpx") or "saveexec" in op or "wrexec" in op:
        return True
    return bool(a) and _is_special(a[0], "exec", "exec_lo", "exec_hi") and not op.startswith(("s_cmp", "s_bitcmp"))


def fold(insns, next_pc, target):
    report = collections.Counter()
    if not insns:
        return insns, report
    pos = {ins["pc"]: n for n, ins in enumerate(insns)}
    targeted = collections.Counter()
    for ins in insns:
        if base_op(ins["op"]) in W.BRANCHES:
            targeted[target(ins)] += 1
    cands = []
    for j, ins in enumerate(insns):
        if base_op(ins["op"]) != "s_cbranch_execnz":
            continue
        t = target(ins)
        i = pos.get(t)
        if i is None or i >= j:
            continue
        report["loops"] += 1
        inner = insns[i:j]
        if any(base_op(x["op"]) in BRANCH_OPS for x in inner):
            report["skip:branch-inside"] += 1
            continue
        if any(targeted.get(x["pc"], 0) for x in insns[i + 1:j + 1]) or targeted[t] != 1:
            report["skip:branch-into"] += 1
            continue
        cands.append((i, j))
    if not cands:
        return insns, report
    blocks, succ, index = _blocks(insns, next_pc, target)
    live_in = W._scalar_liveness([W._active_insns(b) for b in blocks], succ)
    drop, own, guard = set(), set(), set()
    for i, j in cands:
        e = j + 1
        while e < len(insns) and (base_op(insns[e]["op"]) == "s_nop" or base_op(insns[e]["op"]).startswith("s_waitcnt")):
            e += 1
        restore = None
        if e < len(insns):
            x = insns[e]
            a = x["args"]
            if base_op(x["op"]) == "s_mov_b64" and len(a) == 2 and _is_special(a[0], "exec") and a[1].get("k") == "sgpr":
                restore = (1 << a[1]["r"]) | (1 << (a[1]["r"] + 1))
        blk = index.get(insns[j + 1]["pc"]) if j + 1 < len(insns) else None
        live = live_in[blk] if blk is not None else 0
        plan, why = _try(insns, i, j, live, restore, targeted)
        if plan is None:
            report["skip:" + why] += 1
            continue
        report["folded"] += 1
        report["folded_" + plan["form"]] += 1
        report["rfl_folded"] += len(plan["own"])
        report["smem_guarded"] += len(plan["guard"])
        drop.update(plan["drop"])
        own.update(plan["own"])
        guard.update(plan["guard"])
    out = []
    for n, ins in enumerate(insns):
        if n in drop:
            continue
        if n in own:
            ins = dict(ins, own_lane=True)
        elif n in guard:
            ins = dict(ins, exec_guard=True)
        out.append(ins)
    return out, report
