#!/usr/bin/env python3
"""Thread-memory passes (MSL level, round 9).

merge_typed_reads: the translator lowers a typeless image load to a three-way branch on the V# format bits, one typed
read per arm (int / uint / float view of the same descriptor), each arm followed by the same register writes. The
Apple GPU compiler speculates all three reads of every such diamond and schedules them early; in lib_cs_00968 (1,323
reads) that is ~24 KiB of spills per thread, which the driver backs with ~1.4 GiB of scratch per device. The pass
turns each diamond whose arms are identical except for the read into one call of a noinline helper that holds the
branch, followed by one copy of the arm body. The helper returns exactly what the taken arm computed, and the conditions
are pure, so every thread computes the same values as before.

The same holds for sample/gather diamonds; any arm whose body differs, or whose conditions use a variable the arms
assign, is left as it was.

loop_cap: every `for (;;)` loop gets a per-lane trip counter (function scope); a lane leaves the loop after LOOP_CAP
trips of that loop header.
"""
import re
import sys

LOOP_CAP = 1 << 14

_OPS = ("read", "sample", "gather", "sample_compare", "gather_compare")
_TEX_OP = re.compile(r"\b(_\d+)\[(_\d+)\]\.(%s)\(" % "|".join(_OPS))
_WRAP_TOKEN = re.compile(r"(?:as_type<\w+>|(?:u?int|float|half|u?short)[234]?)?\($")
_ASSIGNED = re.compile(r"\b(_\d+)(?:\[[^\]]*\])*(?:\.\w+)?\s*(?:[-+*/&|^%]|<<|>>)?=(?!=)|(?:\+\+|--)(_\d+)|(_\d+)(?:\+\+|--)")


def _close(text, i):
    depth = 0
    for k in range(i, len(text)):
        if text[k] == "(":
            depth += 1
        elif text[k] == ")":
            depth -= 1
            if depth == 0:
                return k
    return -1


def _ops(body):
    """Typed texture ops in `body`: (start, end, view, index, op, args, wrappers outermost first)."""
    out = []
    for m in _TEX_OP.finditer(body):
        e = _close(body, m.end() - 1)
        if e < 0:
            return None
        s, wraps = m.start(), []
        while True:
            w = _WRAP_TOKEN.search(body[:s])
            if not w or (w.group(0) == "(" and w.start() and (body[w.start() - 1].isalnum() or body[w.start() - 1] in "_>")):
                break
            wraps.insert(0, w.group(0)[:-1])
            s = w.start()
        end = e + 1
        for _ in wraps:
            if end >= len(body) or body[end] != ")":
                return None
            end += 1
        out.append((s, end, m.group(1), m.group(2), m.group(3), body[m.end():e], wraps))
    return out


def _views(msl):
    return {m.group(2): m.group(1) for m in re.finditer(r"spvDescriptorArray<(texture\w+<[^>]*>)> (_\d+) \{", msl)}


_ELEM = {"uint": "uint4", "int": "int4", "float": "float4", "half": "half4", "ushort": "ushort4", "short": "short4"}


def _result_type(wraps, view):
    for w in wraps:
        if w.startswith("as_type<"):
            return w[8:-1]
        if w:
            return w
    m = re.match(r"\w+<(\w+)", view)
    return _ELEM.get(m.group(1)) if m else None


def _block(lines, i, ind):
    """lines[i] == ind + '{': returns (body lines, index after the closing brace) or None."""
    if i >= len(lines) or lines[i] != ind + "{":
        return None
    for k in range(i + 1, len(lines)):
        if lines[k] == ind + "}":
            return lines[i + 1:k], k + 1
        if not lines[k].startswith(ind + " ") and lines[k] != "":
            return None
    return None


def _diamond(lines, i):
    m = re.match(r"^( *)if \((.*)\)$", lines[i])
    if not m:
        return None
    ind, c1 = m.group(1), m.group(2)
    a = _block(lines, i + 1, ind)
    if not a or a[1] >= len(lines) or lines[a[1]] != ind + "else":
        return None
    outer = _block(lines, a[1] + 1, ind)
    if not outer:
        return None
    inner, end = outer
    m2 = re.match(r"^( *)if \((.*)\)$", inner[0]) if inner else None
    if not m2 or m2.group(1) != ind + "    ":
        return None
    i2 = ind + "    "
    b = _block(inner, 1, i2)
    if not b or b[1] >= len(inner) or inner[b[1]] != i2 + "else":
        return None
    c = _block(inner, b[1] + 1, i2)
    if not c or c[1] != len(inner):
        return None
    return ind, c1, m2.group(2), a[0], b[0], c[0], end


def merge_typed_reads(msl, noinline=True):
    views = _views(msl)
    helpers, n = {}, 0
    lines = msl.split("\n")
    out, i = [], 0
    while i < len(lines):
        d = _diamond(lines, i) if lines[i].lstrip().startswith("if (") else None
        if d:
            ind, c1, c2, arms = d[0], d[1], d[2], d[3:6]
            bodies = ["\n".join(l.strip() for l in arm) for arm in arms]
            ops = [_ops(bd) for bd in bodies]
            ok = all(o for o in ops) and len({len(o) for o in ops}) == 1
            if ok:
                skel = []
                for bd, o in zip(bodies, ops):
                    t, last = "", 0
                    for s, e, *_ in o:
                        t += bd[last:s] + "@OP"
                        last = e
                    t += bd[last:]
                    for j, loc in enumerate(re.findall(r"^[\w<>]+ (_\d+) = ", t, re.M)):
                        t = re.sub(r"\b%s\b" % loc, "@L%d" % j, t)
                    skel.append(t)
                ok = skel[0] == skel[1] == skel[2]
            if ok:
                assigned = set()
                for bd in bodies:
                    for g in _ASSIGNED.finditer(bd):
                        assigned.update(x for x in g.groups() if x)
                used = set(re.findall(r"\b_\d+\b", c1 + " " + c2))
                for k in range(len(ops[0])):
                    trip = [o[k] for o in ops]
                    if len({(t[3], t[4], t[5]) for t in trip}) != 1:
                        ok = False
                        break
                    if any(t[2] not in views for t in trip):
                        ok = False
                        break
                    types = {_result_type(t[6], views[t[2]]) for t in trip}
                    if len(types) != 1 or None in types:
                        ok = False
                        break
                ok = ok and not (used & assigned)
            if ok:
                body = bodies[0]
                t, last = "", 0
                for k, (s, e, v, idx, op, args, wraps) in enumerate(ops[0]):
                    trip = [o[k] for o in ops]
                    rt = _result_type(wraps, views[v])
                    tail = ""
                    cm = re.search(r",\s*(component::\w+)\s*$", args)
                    if cm:
                        tail, args = cm.group(1), args[:cm.start()]
                    sig = (op + ("|" + tail if tail else ""), rt) + tuple((views[x[2]], tuple(x[6])) for x in trip)
                    name = helpers.setdefault(sig, "spvTypedOp%d" % len(helpers))
                    t += body[last:s] + "%s(%s, %s, %s, %s)" % (
                        name, c1, c2, ", ".join("%s[%s]" % (x[2], idx) for x in trip), args)
                    last = e
                    n += 1
                t += body[last:]
                out.extend(ind + l for l in t.split("\n"))
                i = d[6]
                continue
        out.append(lines[i])
        i += 1
    if not n:
        return msl, 0
    text = "\n".join(out)
    attr = "static __attribute__((noinline))" if noinline else "static inline __attribute__((always_inline))"
    defs = []
    for sig, name in helpers.items():
        op, rt, arms = sig[0], sig[1], sig[2:]

        op, _, tail = op.partition("|")

        def call(k):
            expr = "t%d.%s(args...%s)" % (k, op, ", " + tail if tail else "")
            for w in reversed(arms[k][1]):
                expr = "%s(%s)" % (w, expr)
            return expr
        defs.append("template<typename... A>\n%s %s %s(bool a, bool b, const device %s& t0, const device %s& t1, "
                    "const device %s& t2, A... args)\n{\n    if (a)\n    {\n        return %s;\n    }\n    if (b)\n    {\n"
                    "        return %s;\n    }\n    return %s;\n}\n" % (attr, rt, name, arms[0][0], arms[1][0], arms[2][0],
                                                                    call(0), call(1), call(2)))
    pos = re.search(r"^(kernel|vertex|fragment|\[\[mesh\]\]|\[\[object\]\]) ", text, re.M).start()
    return text[:pos] + "\n".join(defs) + "\n" + text[pos:], n


_FOR = re.compile(r"^( *)for \(;;\)$")


def loop_cap(msl, cap=LOOP_CAP):
    """Every `for (;;)` (SPIRV-Cross prints each structured loop of the translation that way) gets a trip counter
    declared at the top of its function, so it counts every trip of that loop header in one invocation, re-entries
    included; once it passes `cap` the lane leaves the loop at the next trip. A loop of the translation is uniform
    across the emulated wave (exec-mask control), so all lanes of a wave leave at the same trip."""
    lines = msl.split("\n")
    out, decl, n, fn_open = [], {}, 0, None
    for i, line in enumerate(lines):
        if line is None:
            continue
        if line == "{":
            fn_open = len(out)
        m = _FOR.match(line)
        if m and fn_open is not None and i + 1 < len(lines) and lines[i + 1] == m.group(1) + "{":
            ind = m.group(1)
            decl.setdefault(fn_open, []).append("    uint spvLoopTrip%d = 0u;" % n)
            out.append(line)
            out.append(lines[i + 1])
            out.append("%s    if (++spvLoopTrip%d > %du)" % (ind, n, cap))
            out.append("%s    {" % ind)
            out.append("%s        break;" % ind)
            out.append("%s    }" % ind)
            lines[i + 1] = None
            n += 1
            continue
        out.append(line)
    for pos in sorted(decl, reverse=True):
        out[pos + 1:pos + 1] = decl[pos]
    return "\n".join(out), n


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--inline", action="store_true")
    ap.add_argument("--loop-cap", action="store_true")
    a = ap.parse_args()
    t = open(a.src).read()
    t, n = merge_typed_reads(t, noinline=not a.inline)
    c = 0
    if a.loop_cap:
        t, c = loop_cap(t)
    open(a.dst, "w").write(t)
    print("merged %d typed-read diamonds, capped %d loops" % (n, c))
    return 0


if __name__ == "__main__":
    sys.exit(main())
