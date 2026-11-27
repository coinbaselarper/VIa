#!/usr/bin/env python3
"""vshift_query.py EXE FUNC_START FUNC_END AT KEY : reaching definitions of KEY (v6, rax, stk:416) at AT,
and one predecessor chain from AT back to an unknown inflow if there is one."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vshift_analyze as va
img = va.Image(sys.argv[1])
an = va.Analyzer(img)
b, e, at = (int(x, 16) for x in sys.argv[2:5])
key = sys.argv[5]
f = va.Func(img, an.md, b, e)
f.orphans_reachable = True
defs = an.reaching(f, at, key)
for d in sorted(defs, key=str):
    if d == 'ENTRY':
        print('ENTRY')
    else:
        i = f.insns[f.idx[d]]
        print('%x %s %s' % (i.address, i.mnemonic, i.op_str))
if 'ENTRY' in defs:
    # BFS backwards over instructions that do not write key, to the inflow point
    from collections import deque
    prev = {at: None}
    q = deque([at])
    hit = None
    while q and hit is None:
        a = q.popleft()
        k = f.idx[a]
        cands = list(f.preds[a])
        if a == f.entry or not cands:
            hit = a
            break
        for p in cands:
            if p in prev:
                continue
            ins = f.insns[f.idx[p]]
            prev[p] = a
            if an.writes(f, ins, key):
                continue
            q.append(p)
    path = []
    while hit is not None:
        path.append(hit)
        hit = prev[hit]
    jumps = [p for p in path if f.insns[f.idx[p]].mnemonic.startswith('j')]
    print('chain from inflow (jumps only):', ' -> '.join('%x' % p for p in jumps))
    print('inflow at %x' % path[0])
