#!/usr/bin/env python3
"""Census of the wave64 exchange points the fork's emulation emits (--wave64-emulate), by cause.

Every x_round (an exchange: a round in the scheduler, or an inline barrier in structured code), every memory-order
round (x_order_barrier) and every s_barrier is attributed to the instruction and the emulation path that asked for
it. Voted branches that find_local_exec_folds did not fold get the first reason the fold refused them.

  wround_census.py DISASM_DIR OUT.jsonl SPV_OUT_DIR [translator flags...]

Writes one JSON line per shader: {"name", "stage", "lanes", "control_flow", "points": {cause: n},
"refused_folds": {reason: n}, "rfl": {kind: n}, "segs": {seg: cause}}. Translation is the fork's, unchanged.
"""
import json
import os
import sys
from collections import Counter

HERE = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, HERE)
import rdna2spirv as R  # noqa: E402
import wave64_emulate_opt as WO  # noqa: E402

STATE = {}


def _frames():
    f = sys._getframe(2)
    names = []
    while f is not None:
        names.append((f.f_code.co_name, f))
        if f.f_code.co_name in ("translate", "insn", "branch_condition"):
            break
        f = f.f_back
    return names


def _cause(g, kind):
    fr = _frames()
    names = [n for n, _ in fr]
    ins = g.cur_ins
    op = R.base_op(ins["op"]) if ins else "?"
    if kind == "memorder":
        return "memorder:" + op
    if kind == "barrier":
        return "s_barrier"
    if "branch_condition" in names:
        bop = dict(fr)["branch_condition"].f_locals.get("op", "?")
        return "branchvote:" + bop
    if "ds_lane_read" in names:
        return "ds_lane:" + op
    if "ds_permute" in names:
        return "ds_permute"
    if "dpp_insn" in names:
        return "dpp:" + op
    if op.startswith("v_readfirstlane"):
        return "rfl"
    if op.startswith("v_readlane"):
        return "readlane"
    if op.startswith(("v_permlane",)):
        return "permlane:" + op
    for k, tag in (("scc_from_mask", "sccvote"), ("scc_any_half", "sccvote"), ("wave_equal", "scmpvote"),
                   ("quad_any", "wqm"), ("wave_mask", "maskdata"), ("ds_append", "ds_append"),
                   ("desc_lookup", "desc_lookup")):
        if k in names:
            return f"{tag}:{op}"
    return "other:" + op + ":" + "/".join(names[:4])


_orig_x_round = R.Gen.x_round
_orig_x_order = R.Gen.x_order_barrier
_orig_insn = R.Gen.insn


def x_round(self, data=None, bits=None):
    c = _cause(self, "round")
    STATE["points"][c] += 1
    if not self.emulate_inline:
        STATE["segs"][self.x_seg] = c
    else:
        STATE["inline"][c] += 1
    if c == "rfl":
        STATE["rfl_ids"].append(id(self.cur_ins))
    return _orig_x_round(self, data, bits)


def x_order_barrier(self):
    c = _cause(self, "memorder")
    STATE["points"][c] += 1
    if not self.emulate_inline:
        STATE["segs"][self.x_seg] = c
    else:
        STATE["inline"][c] += 1
    return _orig_x_order(self)


def insn(self, ins):
    if self.emulate and R.base_op(ins["op"]) == "s_barrier":
        self.cur_ins = ins
        STATE["points"]["s_barrier"] += 1
        if not self.emulate_inline:
            STATE["segs"][self.x_seg] = "s_barrier"
        else:
            STATE["inline"]["s_barrier"] += 1
    return _orig_insn(self, ins)


_orig_folds = WO.find_local_exec_folds


def find_local_exec_folds(blocks, raw_edges, succ, lane_masks, scc_live, mask_writes=frozenset()):
    res = _orig_folds(blocks, raw_edges, succ, lane_masks, scc_live, mask_writes)
    try:
        STATE["refused"] = refusal_reasons(blocks, raw_edges, succ, lane_masks, scc_live, mask_writes, res[0], res[2])
    except Exception as e:  # diagnostics only
        STATE["refused"] = Counter({"census-error:" + type(e).__name__: 1})
    return res


def _first_forbidden(blk, lane_masks, mask_writes):
    for ins in WO._active_insns(blk):
        m = WO._get_op_meta(ins["op"])
        if m[1]:
            break
        if id(ins) in mask_writes:
            return "parked-mask-write"
        if m[8] & WO._FLAG_FORBIDDEN_FOLD:
            return "cross-lane:" + m[0]
        if WO._vcc_pair_collects_mask(ins, lane_masks):
            return "vcc-mask-collect"
    return None


def refusal_reasons(blocks, raw_edges, succ, lane_masks, scc_live, mask_writes, selected, absorbed):
    """Why each s_cbranch_exec[n]z head was not folded: find_local_exec_folds' filters replayed in order."""
    n = len(blocks)
    reasons = Counter()
    active_blocks = [WO._active_insns(blk) for blk in blocks]
    preds = [set() for _ in range(n)]
    for b in range(n):
        for s in succ[b]:
            preds[s].add(b)
    ipdom = WO._postdominators(succ)
    terms, blk_info, use, kill = [None] * n, [None] * n, [0] * n, [0] * n
    first_bad = [None] * n
    for b, insns in enumerate(active_blocks):
        term = ("fall", None)
        u = k = 0
        for ins in insns:
            m = WO._get_op_meta(ins["op"])
            rd, _, mk = WO._scalar_rw_with_meta(ins, m)
            u |= rd & ~k
            k |= mk
            if m[1]:
                term = ("end", None) if m[0] == "s_endpgm" else ("br", m[0])
                break
        terms[b], use[b], kill[b] = term, u, k
        blk_info[b] = WO._classify_block_body(insns, lane_masks, scc_live, mask_writes)
        if not blk_info[b][0]:
            first_bad[b] = _first_forbidden(blocks[b], lane_masks, mask_writes) or "mask-vote-or-scc"
    live_in = WO._solve_scalar_liveness(use, kill, preds)
    in_sel = set()
    for fd in selected.values():
        in_sel |= fd["region"]
    for b in range(n):
        kind, op = terms[b]
        if kind != "br" or op not in ("s_cbranch_execz", "s_cbranch_execnz", "s_cbranch_vccz", "s_cbranch_vccnz"):
            continue
        if b in selected:
            continue
        if b in in_sel:
            reasons["inside-folded"] += 1
            continue
        if op.startswith("s_cbranch_vcc"):
            reasons["vcc-branch"] += 1
            continue
        edges = raw_edges[b]
        if len(edges) < 2:
            reasons["one-edge"] += 1
            continue
        tgt, fall = edges[0], edges[1]
        run, skip = (fall, tgt) if op == "s_cbranch_execz" else (tgt, fall)
        if run == skip or skip >= n or ipdom.get(b) != skip:
            reasons["loop-backedge" if tgt <= b else "not-postdominated"] += 1
            continue
        region, work, why = set(), [run], None
        while work:
            u = work.pop()
            if u == skip or u in region:
                continue
            if u == b or u == 0 or not succ[u]:
                why = "region-exits"
                break
            if not blk_info[u][0]:
                why = first_bad[u]
                break
            if len(raw_edges[u]) == 1 and terms[u][0] == "br" and terms[u][1] != "s_branch":
                why = "one-edge-branch"
                break
            region.add(u)
            work.extend(s for s in succ[u] if s != skip and s not in region)
        if why is None:
            for u in region:
                if not preds[u] <= (region | ({b} if u == run else set())):
                    why = "multi-entry"
                    break
        if why is None:
            st = [blk_info[u][1] for u in region]
            mem_ops, mem_w = sum(x[0] for x in st), any(x[2] for x in st)
            lds_ops, lds_w = sum(x[3] for x in st), any(x[5] for x in st)
            if (mem_w and mem_ops > 1) or (lds_w and lds_ops > 1):
                why = "memory-writes>1"
            elif (mem_w or lds_w) and WO._region_has_cycle(region, succ):
                why = "memory-write-in-loop"
        if why is None:
            mw = 0
            for u in region:
                mw |= blk_info[u][2]
            hit = mw & live_in[skip]
            if hit:
                names = []
                for bit in range(131):
                    if hit >> bit & 1:
                        names.append("vcc" if bit == 128 else "scc" if bit == 129 else "m0" if bit == 130 else "s")
                why = "live-at-join:" + "+".join(sorted(set(names)))
        if why is None and not WO._exec_writes_stay_inactive(region, blk_info, live_in[run]):
            why = "exec-may-widen"
        if why is None:
            for u in region:
                tk, top = terms[u]
                if tk == "end":
                    why = "inner-endpgm"
                elif tk == "br" and top in ("s_cbranch_vccz", "s_cbranch_vccnz"):
                    why = "inner-vcc-branch"
                elif tk == "br" and top in ("s_cbranch_execz", "s_cbranch_execnz"):
                    why = "inner-unfolded-exec-branch"
                if why:
                    break
        reasons[why or "irreducible-or-overlap"] += 1
    return reasons


R.Gen.x_round = x_round
R.Gen.x_order_barrier = x_order_barrier
R.Gen.insn = insn
WO.find_local_exec_folds = find_local_exec_folds


def main(argv):
    """argv: DISASM_DIR OUT.jsonl SPV_OUT_DIR [translator flags...]"""
    src, out, spv_out, flags = argv[0], argv[1], argv[2], argv[3:]
    orig = R.translate
    fo = open(out, "w")

    def translate(path):
        STATE.clear()
        STATE.update(points=Counter(), inline=Counter(), segs={}, refused=Counter(), rfl_ids=[])
        rec = {"name": os.path.basename(path)[:-2]}
        try:
            text = orig(path)
        except Exception as e:
            rec["error"] = f"{type(e).__name__}: {str(e)[:200]}"
            fo.write(json.dumps(rec) + "\n")
            fo.flush()
            raise
        g = translate.last_gen
        rec.update(stage=g.stage, lanes=getattr(g, "x_lanes", None), emulate=g.emulate,
                   control_flow=getattr(g, "control_flow", None))
        wu = getattr(g, "wu_rfl", frozenset())
        rec["rfl"] = dict(Counter("uniform-proved" if i in wu else "exchanged" for i in STATE["rfl_ids"]))
        rec["points"] = dict(STATE["points"])
        rec["inline"] = dict(STATE["inline"])
        rec["refused_folds"] = dict(STATE["refused"])
        rec["segs"] = {str(k): v for k, v in STATE["segs"].items()}
        fo.write(json.dumps(rec) + "\n")
        fo.flush()
        return text
    R.translate = translate
    sys.argv = ["rdna2spirv.py", src, "--out", spv_out, "--no-asm", "--no-validate"] + flags
    rc = R.main()
    fo.close()
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
