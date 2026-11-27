#!/usr/bin/env python3
"""Register pressure of one AIR function (LLVM text from `metal -S -emit-llvm`): SSA liveness by blocks, then the
live 32-bit words at every instruction. Prints the peak, the pressure profile over the function (by block order) and
what the peak's live set is made of (defining opcode, type), so the values that span the program can be named.

  wired2_live.py FILE.ll [--func main0] [--top 30] [--profile 40]
"""
import argparse
import re
import sys
from collections import Counter

VAL = re.compile(r"%[-a-zA-Z$._0-9]+")
LABEL = re.compile(r"^([-a-zA-Z$._0-9]+):")


def words(ty):
    ty = ty.strip()
    m = re.match(r"<(\d+) x (.+)>$", ty)
    if m:
        return int(m.group(1)) * words(m.group(2))
    if ty in ("i1",):
        return 0.0
    if ty in ("i8", "i16", "half"):
        return 0.5
    if ty in ("i64", "double") or ty.startswith("ptr") or ty.endswith("*"):
        return 2
    if ty.startswith(("{", "<{", "[")):
        return 0
    return 1


def parse(text, func):
    m = re.search(r"^define [^\n]*@" + re.escape(func) + r"\((.*?)\)[^\n]*\{\n(.*?)^\}", text, re.S | re.M)
    if not m:
        sys.exit("no function " + func)
    args, body = m.group(1), m.group(2)
    blocks, order, cur = {}, [], "entry"
    blocks[cur] = []
    order.append(cur)
    for line in body.split("\n"):
        s = line.strip()
        if not s or s.startswith(";"):
            lm = LABEL.match(s) if s else None
            if not lm:
                continue
        lm = LABEL.match(s)
        if lm and not s.startswith("%"):
            cur = lm.group(1)
            blocks[cur] = []
            order.append(cur)
            continue
        blocks[cur].append(s)
    if not blocks["entry"] and len(order) > 1:
        del blocks["entry"]
        order.pop(0)
    return blocks, order


deftext = {}


def analyse(blocks, order):
    ids, size, kind = {}, [], []

    def vid(v):
        if v not in ids:
            ids[v] = len(size)
            size.append(1)
            kind.append("?")
        return ids[v]

    inst = {}
    succ = {}
    phi_uses = {}
    for b in order:
        lst = []
        for s in blocks[b]:
            d = None
            m = re.match(r"(%[-a-zA-Z$._0-9]+) = (.*)$", s)
            rhs = s
            if m:
                d, rhs = m.group(1), m.group(2)
            if rhs.startswith("phi "):
                ty = rhs[4:].split("[", 1)[0]
                for val, pred in re.findall(r"\[\s*([^,\]]+),\s*%([-a-zA-Z$._0-9]+)\s*\]", rhs):
                    if val.startswith("%"):
                        phi_uses.setdefault(pred, set()).add(vid(val))
                di = vid(d)
                size[di] = words(ty)
                kind[di] = "phi " + ty.strip()
                lst.append((di, set(), True))
                continue
            op = rhs.split(" ", 1)[0]
            if d:
                deftext[d] = (b, rhs[:160])
            uses = set()
            body = re.sub(r"!\S+", "", rhs)
            for u in VAL.findall(body):
                if u.startswith("%") and not re.match(r"%\d+$", u) or re.match(r"%\d+$", u) or True:
                    uses.add(u)
            if op == "br" or op == "switch":
                labels = set(re.findall(r"label %([-a-zA-Z$._0-9]+)", rhs))
                succ[b] = labels
                uses = {u for u in uses if u[1:] not in labels}
            uses.discard(d)
            ui = {vid(u) for u in uses}
            di = None
            if d:
                di = vid(d)
                ty = None
                mm = re.match(r"(?:fadd|fsub|fmul|fdiv|add|sub|mul|and|or|xor|shl|lshr|ashr|udiv|sdiv|urem|srem|frem)"
                              r"(?: (?:nuw|nsw|exact|fast|nnan|ninf|nsz|arcp|contract|afn|reassoc|disjoint))* (\S+(?: x \S+>)?)", rhs)
                if op in ("icmp", "fcmp"):
                    ty = "i1"
                elif op == "call":
                    mm2 = re.match(r"(?:tail |notail |musttail )?call (?:[a-z_]+ )*(\S+(?: x \S+>)?) ", rhs)
                    ty = mm2.group(1) if mm2 else "i32"
                elif op in ("load",):
                    ty = rhs.split(" ", 1)[1].split(",", 1)[0]
                    ty = ty.replace("volatile ", "")
                elif op in ("bitcast", "zext", "sext", "trunc", "fptoui", "fptosi", "uitofp", "sitofp", "fpext", "fptrunc",
                            "inttoptr", "ptrtoint", "addrspacecast"):
                    ty = rhs.rsplit(" to ", 1)[1].split(",")[0]
                elif op == "select":
                    ty = rhs.split(",")[1].strip().rsplit(" ", 1)[0]
                elif op == "extractelement":
                    ty = rhs.split("<", 1)[1].split(" x ", 1)[1].split(">", 1)[0]
                elif op in ("insertelement", "shufflevector"):
                    ty = rhs.split(" ", 1)[1].split(" %", 1)[0].split(" <", 1)[0]
                    if op == "shufflevector":
                        mm3 = re.search(r"<(\d+) x i32>", rhs.rsplit(",", 1)[1])
                        inner = re.search(r"<\d+ x ([a-z0-9]+)>", rhs)
                        ty = "<%s x %s>" % (mm3.group(1), inner.group(1)) if mm3 and inner else ty
                elif op == "getelementptr":
                    ty = "ptr"
                elif op in ("extractvalue", "insertvalue", "alloca"):
                    ty = "{}"
                elif mm:
                    ty = mm.group(1)
                else:
                    ty = "i32"
                size[di] = words(ty)
                kind[di] = op + " " + ty
            lst.append((di, ui, False))
        inst[b] = lst
        succ.setdefault(b, set())
    n = len(size)
    ubits, dbits = {}, {}
    for b in order:
        u, dd = 0, 0
        for di, ui, isphi in inst[b]:
            for x in ui:
                if not (dd >> x) & 1:
                    u |= 1 << x
            if di is not None:
                dd |= 1 << di
        for x in phi_uses.get(b, ()):
            pass
        ubits[b], dbits[b] = u, dd
    live_in = {b: 0 for b in order}
    live_out = {b: 0 for b in order}
    phidef = {b: sum(1 << di for di, _, p in inst[b] if p) for b in order}
    changed = True
    rounds = 0
    while changed:
        changed = False
        rounds += 1
        for b in reversed(order):
            out = 0
            for s in succ[b]:
                if s in live_in:
                    out |= live_in[s] & ~phidef[s]
            for x in phi_uses.get(b, ()):
                out |= 1 << x
            inn = ubits[b] | (out & ~dbits[b])
            if out != live_out[b] or inn != live_in[b]:
                live_out[b], live_in[b] = out, inn
                changed = True
    return ids, size, kind, inst, live_out, rounds


_MASKS = {}


def weight(bits, size):
    key = id(size)
    if key not in _MASKS:
        by = {}
        for i, s in enumerate(size):
            if s:
                by[s] = by.get(s, 0) | (1 << i)
        _MASKS[key] = list(by.items())
    return sum(s * bin(bits & m).count("1") for s, m in _MASKS[key])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ll")
    ap.add_argument("--func", default="main0")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--profile", type=int, default=40)
    ap.add_argument("--dump", type=int, default=0)
    a = ap.parse_args()
    blocks, order = parse(open(a.ll).read(), a.func)
    ids, size, kind, inst, live_out, rounds = analyse(blocks, order)
    names = {v: k for k, v in ids.items()}
    sizes_bits = {}
    peak, peak_at, peak_set = -1.0, None, 0
    prof = []
    pos = 0
    sz = size
    for b in order:
        live = live_out[b]
        cur = weight(live, sz)
        bmax = cur
        for idx in range(len(inst[b]) - 1, -1, -1):
            di, ui, isphi = inst[b][idx]
            if di is not None and (live >> di) & 1:
                live &= ~(1 << di)
                cur -= sz[di]
            for x in ui:
                if not (live >> x) & 1:
                    live |= 1 << x
                    cur += sz[x]
            if cur > peak:
                peak, peak_at, peak_set = cur, (b, idx), live
            bmax = max(bmax, cur)
        prof.append((pos, b, len(inst[b]), bmax))
        pos += len(inst[b])
    print("values %d, blocks %d, instructions %d, dataflow rounds %d" % (len(size), len(order), pos, rounds))
    print("peak %.1f live 32-bit words at block %s instruction %d" % (peak, peak_at[0], peak_at[1]))
    c = Counter()
    w = Counter()
    x, i = peak_set, 0
    while x:
        if x & 1:
            k = kind[i].split(" ", 1)
            key = k[0] + (" " + k[1] if len(k) > 1 and k[0] in ("phi", "load", "call") else "")
            c[key] += 1
            w[key] += size[i]
        x >>= 1
        i += 1
    for k, v in w.most_common(a.top):
        print("  %-60s %4d values %7.1f words" % (k[:60], c[k], v))
    if a.dump:
        x, i = peak_set, 0
        rows = []
        while x:
            if x & 1:
                nm = names[i]
                b, t = deftext.get(nm, ("(arg/phi)", kind[i]))
                rows.append((b, nm, t))
            x >>= 1
            i += 1
        bo = {b: k for k, b in enumerate(order)}
        rows.sort(key=lambda r: bo.get(r[0], -1))
        for b, nm, t in rows[:a.dump]:
            print("  live %-10s %-10s %s" % (b[:10], nm[:10], t))
    step = max(1, len(prof) // a.profile)
    print("profile (first instruction index, block, max live words over the next %d blocks):" % step)
    for j in range(0, len(prof), step):
        chunk = prof[j:j + step]
        print("  %7d %-24s %7.1f" % (chunk[0][0], chunk[0][1][:24], max(p[3] for p in chunk)))


if __name__ == "__main__":
    main()
