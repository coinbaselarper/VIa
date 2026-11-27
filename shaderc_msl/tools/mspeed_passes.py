"""MSL passes for the translator's emulated-wave64 programs (mesh and compute), each exact:

chain_segments      In the round loop every invocation runs one segment (a case of the switch over the PC) per round
                    unless the segment sets the chain flag; every round ends in a workgroup barrier. A segment that
                    touches no workgroup memory, makes no barrier, ends no wave and does not wait at s_barrier only
                    changes the invocation's own state, so the next segment may run in the same round. Applied only
                    when the program writes no device memory and no texture (no store another lane could observe
                    without the barrier). Every memory-touching segment still ends its round, so between two of them
                    (in program order of one wave) there is still a barrier; an exchange still starts in one round and
                    resumes in the next, for both halves of the wave alike (the chain decision is per segment, and
                    both halves run the same segments). A thread chains at most 65,536 segments in all, then rounds
                    are as before (the translator's guard).
round_counts_simd   The two per-invocation round counts (atomic adds of 0/1 to one workgroup word each) become one
                    simd_sum per SIMD group and one atomic add by its first lane: the same sums before the barrier.
barrier_tg_only     Workgroup barriers that also order device and texture memory order workgroup memory only, when
                    the program writes neither device memory nor textures.
"""
import re

from mspeed import functions, match_brace, program_function

def device_writes(msl):
    """True when the program may store to device memory or write a texture (conservative)."""
    s, lp, rp, bs, be = functions(msl)["main0"]
    sig = msl[lp:rp]
    if re.search(r"(?<!const )\bdevice\s+(?!const)[\w<>, :]+[&*]\s*\w+\s*\[\[buffer", sig):
        return True
    if re.search(r"\.write\(|\.atomic_|atomic_\w+_explicit\(\s*\(\s*device", msl):
        return True
    if re.search(r"reinterpret_cast<\s*device\s+(?!const)", msl):
        return True
    return False


def _switch(msl):
    prog, pc = program_function(msl)
    s, lp, rp, bs, be = functions(msl)[prog]
    body = msl[bs:be]
    m = re.search(r"\n(\s*)switch \(" + pc + r"\)\n", body)
    sw_open = body.index("{", m.end())
    sw_close = match_brace(body, sw_open)
    return prog, pc, bs, be, body, sw_open, sw_close


def _flags(body, pc):
    m = re.search(r"bool (_\d+) = (_\d+);\n\s*\2 = false;\n\s*if \(\1 && \((_\d+) < 65536u\)\)", body)
    if not m:
        raise ValueError("no chain flag")
    chain = m.group(2)
    d = re.search(r"default:\n\s*\{\n\s*(_\d+) = true;", body)
    done = d.group(1) if d else None
    w = re.search(r"\n\s*(_\d+) = \1 && \(!\(_\d+\[_\d+\] == 0u\)\);", body)
    wait = w.group(1) if w else None
    return chain, done, wait


def _cases(body, sw_open, sw_close):
    out = []
    for m in re.finditer(r"\n(\s*)case (\d+)u:\n\s*\{", body[:sw_close]):
        if m.start() < sw_open:
            continue
        ob = body.index("{", m.end() - 1)
        cb = match_brace(body, ob)
        out.append((int(m.group(2)), ob, cb))
    return out


def chain_segments(msl):
    if device_writes(msl):
        return msl, {"chained": 0, "why": "device or texture writes"}
    prog, pc, bs, be, body, sw_open, sw_close = _switch(msl)
    chain, done, wait = _flags(body, pc)
    s, lp, rp, fbs, fbe = functions(msl)[prog]
    tg = set(re.findall(r"threadgroup [\w<>, :]+&\s*(\w+)", msl[lp:rp]))
    tg |= set(re.findall(r"\n\s*threadgroup [\w<>, :]+ (\w+)(?:\[\d+\])?;", body))
    banned = [re.compile(r"(?<![\w.])" + re.escape(n) + r"\b") for n in tg]
    flags = [f for f in (done, wait) if f]
    budget = "_mspeed_chain"
    edits = []
    stats = {"cases": 0, "chained": 0, "memory": 0, "flags": 0, "other_end": 0, "already": 0}
    for n, ob, cb in _cases(body, sw_open, sw_close):
        stats["cases"] += 1
        text = body[ob:cb]
        if re.search(r"\b" + chain + r" = true;", text):
            stats["already"] += 1
            continue
        if ("atomic_" in text or "threadgroup_barrier" in text or "simdgroup_barrier" in text or "simd_" in text
                or "quad_" in text or any(b.search(text) for b in banned)):
            stats["memory"] += 1
            continue
        if any(re.search(r"\b" + f + r" = ", text) for f in flags):
            stats["flags"] += 1
            continue
        tail = re.search(r"\n(\s*)" + pc + r" = [^;]+;\n(\s*)break;\n\s*\}$", text)
        if not tail or text.count("break;") != 1 or "return" in text or "continue;" in text:
            stats["other_end"] += 1
            continue
        at = ob + tail.start(2) - 1
        ind = tail.group(2)
        edits.append((at, "\n%sif (%s < 65536u)\n%s{\n%s    %s++;\n%s    %s = true;\n%s}" % (ind, budget, ind, ind, budget, ind, chain, ind)))
        stats["chained"] += 1
    for at, ins in sorted(edits, reverse=True):
        body = body[:at] + ins + body[at:]
    first = body.index("{") + 1
    body = body[:first] + "\n    uint %s = 0u;" % budget + body[first:]
    return msl[:bs] + body + msl[be:], stats


def round_counts_simd(msl):
    pat = re.compile(r"\n(\s*)uint (_\d+) = atomic_fetch_add_explicit\(\(threadgroup atomic_uint\*\)&(_\d+)\[(_\d+)\], ([^;]+), memory_order_relaxed\);"
                     r"\n\s*uint (_\d+) = atomic_fetch_add_explicit\(\(threadgroup atomic_uint\*\)&\3\[(_\d+)\], ([^;]+), memory_order_relaxed\);"
                     r"(\n\s*threadgroup_barrier\()")
    m = pat.search(msl)
    if not m:
        return msl, {"round_counts": 0}
    ind, r0, arr, i0, v0, r1, i1, v1, bar = m.groups()
    for r in (r0, r1):
        if len(re.findall(r"\b" + r + r"\b", msl)) != 1:
            return msl, {"round_counts": 0, "why": "count result used"}
    new = ("\n{ind}uint {r0} = simd_sum({v0});\n{ind}uint {r1} = simd_sum({v1});\n{ind}if (simd_is_first())\n{ind}{{\n"
           "{ind}    atomic_fetch_add_explicit((threadgroup atomic_uint*)&{arr}[{i0}], {r0}, memory_order_relaxed);\n"
           "{ind}    atomic_fetch_add_explicit((threadgroup atomic_uint*)&{arr}[{i1}], {r1}, memory_order_relaxed);\n{ind}}}{bar}"
           ).format(ind=ind, r0=r0, r1=r1, v0=v0, v1=v1, arr=arr, i0=i0, i1=i1, bar=bar)
    return msl[:m.start()] + new + msl[m.end():], {"round_counts": 1}


def barrier_tg_only(msl):
    if device_writes(msl):
        return msl, {"barriers_narrowed": 0}
    full = "threadgroup_barrier(mem_flags::mem_device | mem_flags::mem_threadgroup | mem_flags::mem_texture)"
    n = msl.count(full)
    return msl.replace(full, "threadgroup_barrier(mem_flags::mem_threadgroup)"), {"barriers_narrowed": n}


PASSES = {"chain_segments": chain_segments, "round_counts_simd": round_counts_simd, "barrier_tg_only": barrier_tg_only}


def apply(msl, names):
    info = {}
    for n in names:
        msl, i = PASSES[n](msl)
        info[n] = i
    return msl, info


def fp_inline(msl):
    """SPIRV-Cross's NoContraction helpers are [[clang::optnone]] functions, i.e. one call per float add, sub and mul.
    Inlined with contraction and reassociation off in their body, each is still the one operation it computes (the
    same fma), and nothing can fuse or reorder it with its neighbours."""
    n = 0
    out = []
    pos = 0
    for m in re.finditer(r"\[\[clang::optnone\]\] ([^\n]*?)\b(spv\w+)\(([^\n]*)\)\n\{\n", msl):
        out.append(msl[pos:m.start()])
        pragma = "" if m.group(2) == "spvMulExtended" else "#pragma clang fp contract(off) reassociate(off)\n"
        out.append("static inline __attribute__((always_inline)) %s%s(%s)\n{\n%s" % (m.group(1), m.group(2), m.group(3), pragma))
        pos = m.end()
        n += 1
    out.append(msl[pos:])
    return "".join(out), {"helpers_inlined": n}


def typed_reads(msl):
    import tmem
    t, c = tmem.merge_typed_reads(msl)
    return t, {"typed_ops_merged": c}


PASSES["fp_inline"] = fp_inline
PASSES["merge_typed_reads"] = typed_reads


def _sink(msl, arr):
    m = re.search(r"threadgroup spvUnsafeArray<uint, (\d+)>&? " + arr + r"\b", msl)
    return int(m.group(1)) - 1 if m else None


def simd_votes(msl):
    """The exchange's lane votes: every invocation ORs its bit (or 0) into a workgroup word, and lane 0 of each wave
    clears next round's words while every other invocation stores 0 into the sink word (never read). As one OR per SIMD
    group of the OR of its active invocations' values (when they all name the same word; else each its own OR, as
    before) and the clears by the clearing lanes only, the words hold the same values at the barrier."""
    n_or = n_st = 0

    def vote(m):
        nonlocal n_or
        ind, res, arr, idx, val = m.groups()
        if len(re.findall(r"\b" + res + r"\b", msl)) != 1:
            return m.group(0)
        n_or += 1
        p = "(threadgroup atomic_uint*)&%s[_mvi]" % arr
        return ("\n{i}{{\n{i}    uint _mvi = {idx};\n{i}    uint _mvv = {val};\n"
                "{i}    if (simd_all(_mvi == simd_broadcast_first(_mvi)))\n{i}    {{\n{i}        _mvv = simd_or(_mvv);\n"
                "{i}        if (simd_is_first() && _mvv != 0u)\n{i}        {{\n"
                "{i}            atomic_fetch_or_explicit({p}, _mvv, memory_order_relaxed);\n{i}        }}\n{i}    }}\n"
                "{i}    else if (_mvv != 0u)\n{i}    {{\n{i}        atomic_fetch_or_explicit({p}, _mvv, memory_order_relaxed);\n"
                "{i}    }}\n{i}}}").format(i=ind, idx=idx, val=val, p=p)

    msl = re.sub(r"\n(\s*)uint (_\d+) = atomic_fetch_or_explicit\(\(threadgroup atomic_uint\*\)&(_\d+)\[([^;]+?)\], ([^;]+), memory_order_relaxed\);",
                 vote, msl)

    def clear(m):
        nonlocal n_st
        ind, arr, cond, idx, sink = m.groups()
        if _sink(msl, arr) != int(sink):
            return m.group(0)
        n_st += 1
        return ("\n{i}if ({c})\n{i}{{\n{i}    atomic_store_explicit((threadgroup atomic_uint*)&{a}[{x}], 0u, memory_order_relaxed);\n{i}}}"
                ).format(i=ind, c=cond, a=arr, x=idx)

    msl = re.sub(r"\n(\s*)atomic_store_explicit\(\(threadgroup atomic_uint\*\)&(_\d+)\[(_\d+) \? ([^;]+?) : (\d+)u\], 0u, memory_order_relaxed\);",
                 clear, msl)
    return msl, {"votes": n_or, "sink_stores_dropped": n_st}


PASSES["simd_votes"] = simd_votes


def hash_memo(msl):
    """The bindless lookup (FNV-1a of a T# or S#, then up to 8 probes of the table) is a pure function of its key and
    of the table, which no draw changes while it runs (the adapter writes a slot in place only when no submitted work
    can read it). Each invocation keeps its last T# and S# answer and returns it for the same key."""
    from mspeed import add_param, callers_closure
    f = functions(msl)
    hs = [nm for nm, (s, lp, rp, bs, be) in f.items() if "2166136261u" in msl[bs:be] and nm != "main0"]
    if len(hs) != 1:
        return msl, {"memo": 0, "why": "%d hash functions" % len(hs)}
    h = hs[0]
    s, lp, rp, bs, be = f[h]
    plist = msl[lp + 1:rp - 1]
    params = re.findall(r"uint (_\d+)", plist)[:8]
    allp = re.findall(r"(\w+)\s*(?:,|$)", plist)
    if len(params) != 8:
        return msl, {"memo": 0, "why": "hash signature"}
    users = [nm for nm, (s2, lp2, rp2, bs2, be2) in f.items()
             if nm != h and re.search(r"(?<![\w.])" + re.escape(h) + r"\(", msl[bs2:be2])]
    impl = h + "_mspeed"
    msl = msl[:lp - len(h)] + impl + msl[lp:]
    sampler = " && ".join("%s == 4294967295u" % p for p in params[4:])
    call = "%s(%s)" % (impl, ", ".join(allp))
    t = ["", "static inline __attribute__((always_inline))", "uint %s(%s, thread uint* _mm)" % (h, plist), "{",
         "    if (%s)" % sampler, "    {",
         "        if (_mm[13] != 0u && %s)" % " && ".join("_mm[%d] == %s" % (8 + j, p) for j, p in enumerate(params[:4])),
         "        {", "            return _mm[12];", "        }",
         "        uint r = %s;" % call, "        _mm[8] = %s; _mm[9] = %s; _mm[10] = %s; _mm[11] = %s;" % tuple(params[:4]),
         "        _mm[12] = r;", "        _mm[13] = 1u;", "        return r;", "    }",
         "    if (_mm[14] != 0u && %s)" % " && ".join("_mm[%d] == %s" % (j, p) for j, p in enumerate(params)),
         "    {", "        return _mm[15];", "    }",
         "    uint r = %s;" % call]
    t += ["    _mm[%d] = %s;" % (j, p) for j, p in enumerate(params)]
    t += ["    _mm[15] = r;", "    _mm[14] = 1u;", "    return r;", "}"]
    be2 = functions(msl)[impl][4]
    msl = msl[:be2] + "\n".join(t) + msl[be2:]
    wrapper_lp = functions(msl)[h][1]
    out, i = [], 0
    for m in re.finditer(r"(?<![\w.])" + re.escape(h) + r"\(", msl):
        lp2 = m.end() - 1
        if lp2 == wrapper_lp or lp2 < i:
            continue
        from mspeed import match_paren
        rp2 = match_paren(msl, lp2)
        out.append(msl[i:rp2 - 1])
        out.append(", _mm)")
        i = rp2
    out.append(msl[i:])
    msl = "".join(out)
    need = callers_closure(msl, set(users)) - {h, impl, "main0"}
    msl = add_param(msl, sorted(need), "thread uint* _mm", "_mm")
    s, lp, rp, bs, be = functions(msl)["main0"]
    body = msl[bs:be].replace(", _mm)", ", _mspeed_memo)")
    first = body.index("{") + 1
    body = body[:first] + "\n    uint _mspeed_memo[16] = {};" + body[first:]
    msl = msl[:bs] + body + msl[be:]
    return msl, {"memo": 1, "hash": h, "callers": sorted(need)}


PASSES["hash_memo"] = hash_memo


_TAIL = re.compile(
    r"\n(?P<i>\s*)uint (?P<ln>_\d+) = gl_LocalInvocationIndex & 63u;"
    r"\n(?P=i)bool (?P<lo>_\d+) = (?P=ln) < 32u;"
    r"\n(?P=i)uint (?P<bit>_\d+) = (?P<ex>_\d+) \? \(1u << \((?P=ln) & 31u\)\) : 0u;"
    r"\n(?P=i)(?P<xb>_\d+)\[\(\((?P<rd>_\d+) & 1u\) \* 128u\) \+ gl_LocalInvocationIndex\] = (?P<val>_\d+\[\d+u\]);"
    r"\n(?P=i)uint (?P<sl>_\d+) = \(\(\(\((?P=rd) % 3u\) \* 2u\) \+ (?P<wv>_\d+)\) \* 2u\) \+ 256u;"
    r"\n(?P=i)uint (?P<a0>_\d+) = atomic_fetch_or_explicit\(\(threadgroup atomic_uint\*\)&(?P=xb)\[(?P=sl) \+ 0u\], (?P=lo) \? (?P=bit) : 0u, memory_order_relaxed\);"
    r"\n(?P=i)uint (?P<a1>_\d+) = atomic_fetch_or_explicit\(\(threadgroup atomic_uint\*\)&(?P=xb)\[(?P=sl) \+ 1u\], (?P=lo) \? 0u : (?P=bit), memory_order_relaxed\);"
    r"\n(?P=i)(?P<pc>_\d+) = (?P<to>\d+)u;"
    r"\n(?P=i)break;\n\s*\}$")
_HEAD = re.compile(
    r"^\{\n(?P<i>\s*)uint (?P<g>_\d+) = (?P<rd>_\d+) - 1u;"
    r"\n(?P=i)uint (?P<h>_\d+) = \(\(\(\((?P=g) % 3u\) \* 2u\) \+ (?P<wv>_\d+)\) \* 2u\) \+ 256u;"
    r"\n(?P=i)uint (?P<h1>_\d+) = (?P=h) \+ 1u;"
    r"\n(?P=i)(?P<dst>_\d+\[\d+u\]) = (?P<xb>_\d+)\[\(\(\((?P=g) & 1u\) \* 128u\) \+ (?P<lb>_\d+)\) \+ \(\((?P=xb)\[(?P=h)\] != 0u\) \? spvFindLSB\((?P=xb)\[(?P=h)\]\) : \(\((?P=xb)\[(?P=h1)\] != 0u\) \? \(spvFindLSB\((?P=xb)\[(?P=h1)\]\) \+ 32u\) : 0u\)\)\];")


def rfl_batch(msl):
    """Chains of readfirstlane exchanges (a T# or V# gathered dword by dword from VGPRs into SGPRs, one exchange round
    per dword): case X starts the exchange of v0 and resumes in R1, which reads s1 and starts v1, ... Rk reads sk and
    goes on. The values v1.. are VGPRs that R1..Rk-1 do not write, under the same exec mask, so X can write all of them
    at once (v1.. into extra banks of workgroup memory), vote once, and resume in a copy of Rk that reads s1..sk with the
    same first-active-lane choice: the same SGPR values, k - 1 rounds fewer. R1.. stay for any other predecessor."""
    prog, pc, bs, be, body, so, sc = _switch(msl)
    cases = {n: (ob, cb) for n, ob, cb in _cases(body, so, sc)}
    text = {n: body[ob:cb] for n, (ob, cb) in cases.items()}
    tails = {n: _TAIL.search(t) for n, t in text.items()}
    heads = {n: _HEAD.search(t) for n, t in text.items()}
    link = {}
    for n, t in text.items():
        h, tl = heads[n], tails[n]
        if h and tl and h.end() == tl.start() and tl.group("ex") == tails[n].group("ex"):
            link[n] = tl
    chains = []
    for n, tl in tails.items():
        if not tl or n in link:
            continue
        seq = [n]
        cur = int(tl.group("to"))
        vals = [tl.group("val")]
        dsts = []
        ok = True
        while cur in link and len(seq) < 16:
            l = link[cur]
            if l.group("ex") != tl.group("ex") or l.group("xb") != tl.group("xb") or l.group("rd") != tl.group("rd"):
                ok = False
                break
            dsts.append(heads[cur].group("dst"))
            vals.append(l.group("val"))
            seq.append(cur)
            cur = int(l.group("to"))
        if not ok or len(seq) < 2 or cur not in heads or not heads[cur] or cur in link:
            continue
        written = set()
        for r in seq[1:]:
            written.add(heads[r].group("dst"))
        if any(v in written for v in vals):
            continue
        dsts.append(heads[cur].group("dst"))
        chains.append((n, seq, cur, vals, dsts))
    if not chains:
        return msl, {"chains": 0}
    kmax = max(len(c[3]) for c in chains)
    bank = "_mspeed_xb"
    nxt = max(cases) + 1
    edits = []
    new_cases = []
    for n, seq, last, vals, dsts in chains:
        tl = tails[n]
        i = tl.group("i")
        writes = "".join("\n%s%s[%du + ((%s & 1u) * 128u) + gl_LocalInvocationIndex] = %s;" % (i, bank, 256 * (j - 1), tl.group("rd"), v)
                         for j, v in enumerate(vals) if j)
        g = tl.group(0)
        g2 = g.replace("\n%s%s = %su;" % (i, tl.group("pc"), tl.group("to")), "\n%s%s = %du;" % (i, tl.group("pc"), nxt))
        g2 = g2.replace("\n%s%s[((%s & 1u) * 128u) + gl_LocalInvocationIndex] = %s;" % (i, tl.group("xb"), tl.group("rd"), vals[0]),
                        "\n%s%s[((%s & 1u) * 128u) + gl_LocalInvocationIndex] = %s;%s" % (i, tl.group("xb"), tl.group("rd"), vals[0], writes))
        ob, cb = cases[n]
        edits.append((ob + tl.start(), ob + tl.end(), g2))
        h = heads[last]
        hi = h.group("i")
        sel = "_mspeed_sel%d" % nxt
        reads = ["%suint %s = %s - 1u;" % (hi, h.group("g"), h.group("rd")),
                 "%suint %s = ((((%s %% 3u) * 2u) + %s) * 2u) + 256u;" % (hi, h.group("h"), h.group("g"), h.group("wv")),
                 "%suint %s = %s + 1u;" % (hi, h.group("h1"), h.group("h")),
                 "%suint %s = ((%s & 1u) * 128u) + %s + ((%s[%s] != 0u) ? spvFindLSB(%s[%s]) : ((%s[%s] != 0u) ? (spvFindLSB(%s[%s]) + 32u) : 0u));"
                 % (hi, sel, h.group("g"), h.group("lb"), h.group("xb"), h.group("h"), h.group("xb"), h.group("h"),
                    h.group("xb"), h.group("h1"), h.group("xb"), h.group("h1"))]
        for j, d in enumerate(dsts):
            src = "%s[%s]" % (h.group("xb"), sel) if j == 0 else "%s[%du + %s]" % (bank, 256 * (j - 1), sel)
            reads.append("%s%s = %s;" % (hi, d, src))
        rest = text[last][h.end():]
        case_ind = hi[:-4]
        new_cases.append("\n%scase %du:\n%s{\n%s%s" % (case_ind, nxt, case_ind, "\n".join(reads), rest))
        nxt += 1
    for a, b, rep in sorted(edits, reverse=True):
        body = body[:a] + rep + body[b:]
    prog2, pc2, bs2, be2, body2, so2, sc2 = prog, pc, bs, be, body, so, sc
    m = re.search(r"\n(\s*)default:\n", body)
    body = body[:m.start()] + "".join(new_cases) + body[m.start():]
    msl = msl[:bs] + body + msl[be:]
    from mspeed import add_param
    msl = add_param(msl, [prog], "threadgroup uint* %s" % bank, bank)
    s, lp, rp, mbs, mbe = functions(msl)["main0"]
    mb = msl[mbs:mbe]
    first = mb.index("{") + 1
    mb = mb[:first] + "\n    threadgroup uint %s[%d];" % (bank, 256 * (kmax - 1)) + mb[first:]
    msl = msl[:mbs] + mb + msl[mbe:]
    return msl, {"chains": len(chains), "rounds_saved_per_pass": sum(len(c[1]) - 1 for c in chains), "kmax": kmax,
                 "bank_bytes": 1024 * (kmax - 1)}


PASSES["rfl_batch"] = rfl_batch
