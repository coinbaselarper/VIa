"""MSL passes for translator-fork output (sstruct.py), exact by the same argument as mspeed_passes.py.

chain_segments_all  mspeed_passes.chain_segments over every switch on the PC, not only the first: with
                    --barrier-tail a program has a round loop per mini-scheduler plus the tail scheduler, each with
                    its own chain loop on the same chain flag. A segment that touches no workgroup memory, makes no
                    barrier or atomic, uses no SIMD or quad op, sets no loop flag (done, region left, waiting at
                    s_barrier) and ends in one PC store may let the next segment run in the same round. Only when
                    the program writes no device memory and no texture.
"""
import re

from mspeed import functions, match_brace, program_function
from mspeed_passes import device_writes


def chain_segments_all(msl):
    if device_writes(msl):
        return msl, {"chained": 0, "why": "device or texture writes"}
    prog, pc = program_function(msl)
    s, lp, rp, bs, be = functions(msl)[prog]
    body = msl[bs:be]
    m = re.search(r"bool (_\d+) = (_\d+);\n\s*\2 = false;\n\s*if \(\1 && \((_\d+) < 65536u\)\)", body)
    if not m:
        raise ValueError("no chain flag")
    chain = m.group(2)
    flags = set(re.findall(r"default:\n\s*\{\n\s*(_\d+) = true;", body))
    flags |= set(re.findall(r"\n\s*(_\d+) = \1 && \(!\(_\d+\[_\d+\] == 0u\)\);", body))
    flags |= set(re.findall(r"\b(_\d+) = true;\n\s*(?:_\d+ = [^;]+;\n\s*)*break;\n\s*\}\n\s*default:", body))
    tg = set(re.findall(r"threadgroup [\w<>, :]+&\s*(\w+)", msl[lp:rp]))
    tg |= set(re.findall(r"\n\s*threadgroup [\w<>, :]+ (\w+)(?:\[\d+\])?;", body))
    banned = [re.compile(r"(?<![\w.])" + re.escape(n) + r"\b") for n in tg]
    budget = "_sstruct_chain"
    edits = []
    stats = {"switches": 0, "cases": 0, "chained": 0, "memory": 0, "flags": 0, "other_end": 0, "already": 0}
    for sw in re.finditer(r"\n(\s*)switch \(" + pc + r"\)\n", body):
        stats["switches"] += 1
        sw_open = body.index("{", sw.end())
        sw_close = match_brace(body, sw_open)
        for cm in re.finditer(r"\n(\s*)case (\d+)u:\n\s*\{", body[:sw_close]):
            if cm.start() < sw_open:
                continue
            ob = body.index("{", cm.end() - 1)
            cb = match_brace(body, ob)
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
            edits.append((at, "\n%sif (%s < 65536u)\n%s{\n%s    %s++;\n%s    %s = true;\n%s}"
                          % (ind, budget, ind, ind, budget, ind, chain, ind)))
            stats["chained"] += 1
    for at, ins in sorted(edits, reverse=True):
        body = body[:at] + ins + body[at:]
    first = body.index("{") + 1
    body = body[:first] + "\n    uint %s = 0u;" % budget + body[first:]
    return msl[:bs] + body + msl[be:], stats
