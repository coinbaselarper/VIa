"""SPIR-V reading and the pre-passes of the shaderc_msl recipe (pure Python, no third-party code).

Passes (each returns new words plus a report dict; all are no-ops when nothing applies):
  split_bindings      set-0 binding b -> descriptor set 1 + b, binding 0 (one argument buffer per contract binding)
  lower_vertex_lane   vertex shaders: SubgroupLocalInvocationId -> Private uint set to
                      OpGroupNonUniformIAdd(ExclusiveScan, 1) at entry (MSL: simd_prefix_exclusive_sum(1u))
  drop_outputs        mesh/vertex shaders: remove user outputs at the given locations (stores deleted)
"""
import struct

OP_NAME, OP_ENTRY, OP_EXECMODE, OP_CAPABILITY = 5, 15, 16, 17
OP_TYPE_INT, OP_TYPE_FLOAT, OP_TYPE_VECTOR, OP_TYPE_ARRAY, OP_TYPE_STRUCT, OP_TYPE_POINTER = 21, 22, 23, 28, 30, 32
OP_TYPE_BOOL = 20
OP_CONSTANT, OP_FUNCTION, OP_VARIABLE, OP_LOAD, OP_STORE, OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN = 43, 54, 59, 61, 62, 65, 66
OP_DECORATE, OP_MEMBER_DECORATE, OP_LABEL, OP_MEMBER_NAME = 71, 72, 248, 6
OP_GNU_IADD = 349
DEC_BUILTIN, DEC_LOCATION, DEC_BINDING, DEC_SET, DEC_COMPONENT, DEC_PER_PRIM = 11, 30, 33, 34, 31, 5271
SC_INPUT, SC_OUTPUT, SC_PRIVATE, SC_FUNCTION, SC_WORKGROUP = 1, 3, 6, 7, 4
BI_SUBGROUP_LOCAL_ID, BI_SUBGROUP_SIZE = 41, 36
CAP_GNU_ARITH = 63
MODELS = {0: "vs", 1: "tcs", 2: "tes", 3: "gs", 4: "ps", 5: "cs", 5364: "as", 5365: "ms"}


def load(path):
    raw = open(path, "rb").read()
    return list(struct.unpack("<%dI" % (len(raw) // 4), raw))


def save(words, path):
    open(path, "wb").write(struct.pack("<%dI" % len(words), *words))


def to_bytes(words):
    return struct.pack("<%dI" % len(words), *words)


def insts(words):
    i = 5
    n_words = len(words)
    while i < n_words:
        n = words[i] >> 16
        if n == 0:
            raise ValueError("bad SPIR-V word count at %d" % i)
        yield i, words[i] & 0xFFFF, n
        i += n


def lit_str(words, start):
    b = bytearray()
    for w in words[start:]:
        bb = struct.pack("<I", w)
        if 0 in bb:
            b += bb[:bb.index(0)]
            break
        b += bb
    return b.decode("utf-8", "replace")


class Module:
    def __init__(self, words):
        self.words = words
        self.types, self.consts, self.vars, self.decor, self.names = {}, {}, {}, {}, {}
        self.member_decor = {}
        self.entry, self.model, self.caps = None, None, set()
        self.exec_modes = []
        for i, op, n in insts(words):
            w = words[i:i + n]
            if op == OP_CAPABILITY:
                self.caps.add(w[1])
            elif op == OP_ENTRY:
                self.model = MODELS.get(w[1], str(w[1]))
                name_words = 0
                for k in range(3, n):
                    name_words += 1
                    if 0 in struct.pack("<I", w[k]):
                        break
                self.entry = {"pos": i, "fn": w[2], "iface": list(w[3 + name_words:])}
            elif op == OP_EXECMODE:
                self.exec_modes.append(list(w[1:]))
            elif op == OP_NAME:
                self.names[w[1]] = lit_str(words, i + 2)
            elif op == OP_DECORATE:
                self.decor.setdefault(w[1], []).append((w[2], list(w[3:]), i))
            elif op == OP_MEMBER_DECORATE:
                self.member_decor.setdefault(w[1], []).append((w[2], w[3], list(w[4:]), i))
            elif op in (OP_TYPE_INT, OP_TYPE_FLOAT, OP_TYPE_VECTOR, OP_TYPE_ARRAY, OP_TYPE_STRUCT, OP_TYPE_POINTER, OP_TYPE_BOOL):
                self.types[w[1]] = (op, list(w[2:]), i)
            elif op == OP_CONSTANT:
                self.consts[w[2]] = (w[1], list(w[3:]), i)
            elif op == OP_VARIABLE and i < self.first_function():
                self.vars[w[2]] = (w[1], w[3], i)

    def first_function(self):
        if not hasattr(self, "_ff"):
            self._ff = len(self.words)
            for i, op, n in insts(self.words):
                if op == OP_FUNCTION:
                    self._ff = i
                    break
        return self._ff

    def dec(self, ident, kind):
        for k, args, _ in self.decor.get(ident, []):
            if k == kind:
                return args[0] if args else True
        return None

    def scalars(self, tid):
        op, a, _ = self.types[tid]
        if op in (OP_TYPE_INT, OP_TYPE_FLOAT, OP_TYPE_BOOL):
            return 1
        if op == OP_TYPE_VECTOR:
            return a[1] * self.scalars(a[0])
        if op == OP_TYPE_ARRAY:
            return self.consts[a[1]][1][0] * self.scalars(a[0])
        if op == OP_TYPE_STRUCT:
            return sum(self.scalars(m) for m in a)
        raise ValueError("no scalar count for type op %d" % op)

    def pointee(self, var):
        return self.types[self.vars[var][0]][1][1]

    def interface(self):
        """User and builtin inputs/outputs: [{id, sc, location, component, builtin, scalars, per_prim, arrayed}]"""
        out = []
        for v, (ptype, sc, _) in self.vars.items():
            if sc not in (SC_INPUT, SC_OUTPUT):
                continue
            t = self.pointee(v)
            arrayed = False
            per = 1
            if self.model in ("ms", "gs", "tcs") or (self.model in ("tes",) and sc == SC_INPUT):
                op, a, _ = self.types[t]
                if op == OP_TYPE_ARRAY and (self.model != "gs" or sc == SC_INPUT):
                    per = self.consts[a[1]][1][0]
                    t = a[0]
                    arrayed = True
            d = {"id": v, "sc": "in" if sc == SC_INPUT else "out", "location": self.dec(v, DEC_LOCATION),
                 "component": self.dec(v, DEC_COMPONENT) or 0, "builtin": self.dec(v, DEC_BUILTIN),
                 "per_prim": bool(self.dec(v, DEC_PER_PRIM)), "arrayed": arrayed, "count": per,
                 "name": self.names.get(v, "")}
            try:
                d["scalars"] = self.scalars(t)
            except (KeyError, ValueError):
                d["scalars"] = None
            op_t, a_t, _ = self.types.get(t, (None, None, None))
            if op_t == OP_TYPE_STRUCT and t in self.member_decor:
                mem = {}
                for idx, kind, args, _ in self.member_decor[t]:
                    if kind == DEC_BUILTIN:
                        mem[idx] = args[0]
                d["members"] = [{"builtin": mem.get(i), "scalars": self.scalars(mt)} for i, mt in enumerate(a_t)]
            out.append(d)
        return sorted(out, key=lambda d: (d["sc"], d["location"] if d["location"] is not None else 999, d["id"]))

    def workgroup_bytes(self):
        tot = 0
        per = {}
        for v, (ptype, sc, _) in self.vars.items():
            if sc == SC_WORKGROUP:
                try:
                    b = 4 * self.scalars(self.pointee(v))
                except (KeyError, ValueError):
                    b = None
                per[self.names.get(v, str(v))] = b
                tot += b or 0
        return tot, per


def _rebuild(words, drop=(), insert_before=None, patch=None):
    """Return new words with instruction starts in `drop` removed, `insert_before[pos]` (lists of words) inserted
    before the instruction at pos, and `patch[(pos, k)] = value` applied."""
    patch = patch or {}
    insert_before = insert_before or {}
    out = words[:5]
    for i, op, n in insts(words):
        for ins in insert_before.get(i, []):
            out += ins
        if i in drop:
            continue
        w = words[i:i + n]
        for k in range(n):
            if (i, k) in patch:
                w[k] = patch[(i, k)]
        out += w
    for ins in insert_before.get(len(words), []):
        out += ins
    return out


def split_bindings(words):
    out = list(words)
    sets, binds = {}, {}
    for i, op, n in insts(out):
        if op == OP_DECORATE and n >= 4:
            if out[i + 2] == DEC_SET:
                sets[out[i + 1]] = i + 3
            elif out[i + 2] == DEC_BINDING:
                binds[out[i + 1]] = i + 3
    moved = []
    for var, pos in sets.items():
        if out[pos] == 0 and var in binds:
            b = out[binds[var]]
            out[pos] = 1 + b
            out[binds[var]] = 0
            moved.append(b)
    return out, {"moved": sorted(set(moved)), "nonzero_sets": sorted({out[p] for v, p in sets.items() if v not in binds or out[p] == 0} - {0})}


def _inst(op, *args):
    return [((len(args) + 1) << 16) | op] + list(args)


def lower_vertex_lane(words):
    m = Module(words)
    if m.model != "vs":
        return words, {"applied": False}
    lane = [v for v, (_, sc, _) in m.vars.items() if sc == SC_INPUT and m.dec(v, DEC_BUILTIN) == BI_SUBGROUP_LOCAL_ID]
    size = [v for v, (_, sc, _) in m.vars.items() if sc == SC_INPUT and m.dec(v, DEC_BUILTIN) == BI_SUBGROUP_SIZE]
    if not lane and not size:
        return words, {"applied": False}
    bound = words[3]

    def new_id():
        nonlocal bound
        bound += 1
        return bound - 1

    uint_t = next((t for t, (op, a, _) in m.types.items() if op == OP_TYPE_INT and a == [32, 0]), None)
    if uint_t is None:
        raise ValueError("no uint type")
    pre = []

    def const(val):
        for c, (t, a, _) in m.consts.items():
            if t == uint_t and a == [val]:
                return c
        c = new_id()
        pre.append(_inst(OP_CONSTANT, uint_t, c, val))
        m.consts[c] = (uint_t, [val], None)
        return c

    ptr_priv = next((t for t, (op, a, _) in m.types.items() if op == OP_TYPE_POINTER and a == [SC_PRIVATE, uint_t]), None)
    if ptr_priv is None:
        ptr_priv = new_id()
        pre.append(_inst(OP_TYPE_POINTER, ptr_priv, SC_PRIVATE, uint_t))
    c1, c3, c32 = const(1), const(3), const(32)
    drop, patch, ins = set(), {}, {}
    targets = lane + size
    first_var = min(m.vars[v][2] for v in targets)
    ins[first_var] = pre
    for v in targets:
        for k, args, pos in m.decor.get(v, []):
            if k == DEC_BUILTIN:
                drop.add(pos)
        vpos = m.vars[v][2]
        patch[(vpos, 1)] = ptr_priv
        patch[(vpos, 3)] = SC_PRIVATE
    fn_pos = None
    label_pos = None
    after_vars = None
    for i, op, n in insts(words):
        if op == OP_FUNCTION and words[i + 2] == m.entry["fn"]:
            fn_pos = i
            continue
        if fn_pos is not None and label_pos is None and op == OP_LABEL:
            label_pos = i
            continue
        if label_pos is not None:
            if op == OP_VARIABLE:
                continue
            after_vars = i
            break
    body = []
    for v in lane:
        t = new_id()
        body += _inst(OP_GNU_IADD, uint_t, t, c3, 2, c1)
        body += _inst(OP_STORE, v, t)
    for v in size:
        body += _inst(OP_STORE, v, c32)
    ins.setdefault(after_vars, []).append(body)
    out = _rebuild(words, drop, ins, patch)
    if CAP_GNU_ARITH not in m.caps:
        out = out[:5] + _inst(OP_CAPABILITY, CAP_GNU_ARITH) + out[5:]
    out[3] = bound
    return out, {"applied": True, "lane_vars": len(lane), "size_vars": len(size)}


OP_COPY_MEMORY, OP_FUNCTION_CALL, OP_COPY_OBJECT, OP_PHI, OP_SELECT, OP_PTR_ACCESS_CHAIN = 63, 57, 83, 245, 169, 67


def pointer_uses(words, ids):
    """Ids (variables or pointers derived from them) used by anything other than OpStore-to / OpAccessChain-of."""
    hit = set()
    for i, op, n in insts(words):
        if op == OP_LOAD and words[i + 3] in ids:
            hit.add(words[i + 3])
        elif op == OP_COPY_MEMORY:
            hit.update(x for x in words[i + 1:i + 3] if x in ids)
        elif op == OP_FUNCTION_CALL:
            hit.update(x for x in words[i + 4:i + n] if x in ids)
        elif op in (OP_COPY_OBJECT, OP_SELECT):
            hit.update(x for x in words[i + 3:i + n] if x in ids)
        elif op == OP_PHI:
            hit.update(x for x in words[i + 3:i + n:2] if x in ids)
        elif op == OP_STORE and words[i + 2] in ids:
            hit.add(words[i + 2])
        elif op == OP_PTR_ACCESS_CHAIN and words[i + 3] in ids:
            hit.add(words[i + 3])
        elif 227 <= op <= 242 and words[i + 3] in ids:
            hit.add(words[i + 3])
    return hit


def drop_outputs(words, locations):
    """Delete user outputs at `locations` (whole variables). Returns words and the dropped locations."""
    m = Module(words)
    locations = set(locations)
    victims = {d["id"]: d["location"] for d in m.interface()
               if d["sc"] == "out" and d["builtin"] is None and d["location"] in locations}
    if not victims:
        return words, {"dropped": []}
    chains = {}
    for i, op, n in insts(words):
        if op in (OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN) and words[i + 3] in victims:
            chains[words[i + 2]] = words[i + 3]
    drop = set()
    chain_pos = {}
    for i, op, n in insts(words):
        if op == OP_STORE and (words[i + 1] in victims or words[i + 1] in chains):
            drop.add(i)
        elif op in (OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN) and words[i + 3] in victims:
            drop.add(i)
            chain_pos[words[i + 2]] = i
    keep_private = {chains.get(x, x) for x in pointer_uses(words, set(victims) | set(chains))}
    if keep_private:
        drop = {p for p in drop if not (words[p] & 0xFFFF == OP_STORE and chains.get(words[p + 1], words[p + 1]) in keep_private)
                and not (words[p] & 0xFFFF in (OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN) and words[p + 3] in keep_private)}
    patch = {}
    priv_ptrs = {}
    pre = []
    bound = words[3]
    for v in victims:
        for k, args, pos in m.decor.get(v, []):
            drop.add(pos)
        if v in keep_private:
            pt = m.vars[v][0]
            pointee = m.types[pt][1][1]
            if pointee not in priv_ptrs:
                priv_ptrs[pointee] = bound
                pre.append(_inst(OP_TYPE_POINTER, bound, SC_PRIVATE, pointee))
                bound += 1
            patch[(m.vars[v][2], 1)] = priv_ptrs[pointee]
            patch[(m.vars[v][2], 3)] = SC_PRIVATE
            for cid, base in chains.items():
                if base != v:
                    continue
                cp = chain_pos[cid]
                el = m.types[words[cp + 1]][1][1]
                if el not in priv_ptrs:
                    priv_ptrs[el] = bound
                    pre.append(_inst(OP_TYPE_POINTER, bound, SC_PRIVATE, el))
                    bound += 1
                patch[(cp, 1)] = priv_ptrs[el]
        else:
            drop.add(m.vars[v][2])
    ins = {}
    if pre:
        ins[min(m.vars[v][2] for v in victims)] = pre
    ep = m.entry["pos"]
    n_ep = words[ep] >> 16
    ep_words = words[ep:ep + n_ep]
    name_end = 3 + (n_ep - 3 - len(m.entry["iface"]))
    gone = {v for v in victims if v not in keep_private}
    new_ep = ep_words[:name_end] + [x for x in m.entry["iface"] if x not in gone]
    new_ep[0] = (len(new_ep) << 16) | OP_ENTRY
    drop.add(ep)
    ins.setdefault(ep, []).append(new_ep)
    for i, op, n in insts(words):
        if op == OP_NAME and words[i + 1] in gone:
            drop.add(i)
    out = _rebuild(words, drop, ins, patch)
    out[3] = bound
    return out, {"dropped": sorted(victims.values()), "kept_private": sorted(victims[v] for v in keep_private)}


DEC_INDEX = 32


def dual_source(words):
    """Fragment shaders of dual-source-blend PSOs: the console exports blend source 1 as MRT1, so the translation
    writes it at Location 1; Metal (and Vulkan) take it from Location 0 Index 1. Moves the Location 1 output there.
    Returns words and {"moved": bool}."""
    m = Module(words)
    if m.model != "ps":
        return words, {"moved": False}
    outs = {d["location"]: d["id"] for d in m.interface() if d["sc"] == "out" and d["builtin"] is None}
    if 0 not in outs or 1 not in outs:
        return words, {"moved": False}
    target = outs[1]
    patch, ins = {}, {}
    for i, op, n in insts(words):
        if op == OP_DECORATE and n >= 4 and words[i + 1] == target and words[i + 2] == DEC_LOCATION:
            patch[(i, 3)] = 0
            ins[i] = [_inst(OP_DECORATE, target, DEC_INDEX, 1)]
            break
    if not patch:
        return words, {"moved": False}
    return _rebuild(words, (), ins, patch), {"moved": True}


def trim_outputs(words, widths):
    """Shrink user outputs (vec4 or arrayed vec4) at location L to widths[L] components; stores keep the leading
    components. A variable used other than through element access chains and stores is left alone."""
    m = Module(words)
    want = {d["id"]: widths[d["location"]] for d in m.interface()
            if d["sc"] == "out" and d["builtin"] is None and d["location"] in widths and 0 < widths[d["location"]] < 4}
    if not want:
        return words, {"trimmed": {}}
    chains, stores, bad = {}, {}, set()
    for i, op, n in insts(words):
        if op in (OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN) and words[i + 3] in want:
            if n != 5:
                bad.add(words[i + 3])
            chains[words[i + 2]] = (words[i + 3], i)
    for i, op, n in insts(words):
        if op == OP_STORE and words[i + 1] in chains:
            stores.setdefault(words[i + 1], []).append(i)
    for x in pointer_uses(words, set(want) | set(chains)):
        bad.add(chains[x][0] if x in chains else x)
    for i, op, n in insts(words):
        if op == OP_STORE and words[i + 1] in want:
            bad.add(words[i + 1])
    bound = words[3]
    patch, ins = {}, {}
    vec_types = {(a[0], a[1]): t for t, (op, a, _) in m.types.items() if op == OP_TYPE_VECTOR}
    ptr_types = {(a[0], a[1]): t for t, (op, a, _) in m.types.items() if op == OP_TYPE_POINTER}
    arr_types = {(a[0], a[1]): t for t, (op, a, _) in m.types.items() if op == OP_TYPE_ARRAY}
    done = {}
    for v, w in want.items():
        if v in bad:
            continue
        pointee = m.pointee(v)
        op, a, _ = m.types[pointee]
        arr_len = None
        elem = pointee
        if op == OP_TYPE_ARRAY:
            elem, arr_len = a[0], a[1]
        eop, ea, _ = m.types[elem]
        if eop != OP_TYPE_VECTOR or ea[1] != 4:
            continue
        comp = ea[0]
        if arr_len is None:
            continue
        pre = []

        def get(table, key, make):
            nonlocal bound
            if key in table:
                return table[key]
            t = bound
            bound += 1
            pre.append(make(t))
            table[key] = t
            return t

        nt = comp if w == 1 else get(vec_types, (comp, w), lambda t: _inst(OP_TYPE_VECTOR, t, comp, w))
        if arr_len is not None:
            at = get(arr_types, (nt, arr_len), lambda t: _inst(OP_TYPE_ARRAY, t, nt, arr_len))
        else:
            at = nt
        pv = get(ptr_types, (SC_OUTPUT, at), lambda t: _inst(OP_TYPE_POINTER, t, SC_OUTPUT, at))
        pe = get(ptr_types, (SC_OUTPUT, nt), lambda t: _inst(OP_TYPE_POINTER, t, SC_OUTPUT, nt))
        vpos = m.vars[v][2]
        if pre:
            ins.setdefault(vpos, []).extend(pre)
        patch[(vpos, 1)] = pv
        for cid, (base, cpos) in chains.items():
            if base != v:
                continue
            patch[(cpos, 1)] = pe
            for spos in stores.get(cid, []):
                val = words[spos + 2]
                t = bound
                bound += 1
                if w == 1:
                    ins.setdefault(spos, []).append(_inst(81, comp, t, val, 0))
                else:
                    ins.setdefault(spos, []).append(_inst(79, nt, t, val, val, *range(w)))
                patch[(spos, 2)] = t
        done[m.dec(v, DEC_LOCATION)] = w
    if not done:
        return words, {"trimmed": {}}
    out = _rebuild(words, set(), ins, patch)
    out[3] = bound
    return out, {"trimmed": done}


OP_ULESS = 176
OP_ATOMIC_FIRST, OP_ATOMIC_LAST = 227, 242


def shrink_lds(words, dwords):
    """Compute shaders: shrink the largest Workgroup uint array (the translator's LDS) to `dwords` plus two sink
    words, and redirect every element access at or past `dwords` to a sink: loads to a word nothing writes (reads 0),
    stores and atomics to another. RDNA2 bounds-checks LDS at the RSRC2 allocation the same way (reads 0, drops
    writes); the translator does the same under --max-shared-memory (lds_bound)."""
    m = Module(words)
    best = None
    for v, (pt, sc, pos) in m.vars.items():
        if sc != SC_WORKGROUP:
            continue
        t = m.types[pt][1][1]
        op, a, _ = m.types[t]
        if op != OP_TYPE_ARRAY:
            continue
        eop, ea, _ = m.types[a[0]]
        if eop != OP_TYPE_INT or ea[0] != 32:
            continue
        n = m.consts[a[1]][1][0]
        if best is None or n > best[1]:
            best = (v, n, a[0], pos)
    if best is None or best[1] <= dwords:
        return words, {"applied": False}
    var, old_n, elem, vpos = best
    chains = {}
    for i, op, n in insts(words):
        if op in (OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN) and words[i + 3] == var:
            if n != 5:
                return words, {"applied": False, "why": "multi-index access chain"}
            chains[words[i + 2]] = i
    if pointer_uses(words, {var}):
        return words, {"applied": False, "why": "LDS variable used directly"}
    writes = set()
    for i, op, n in insts(words):
        if op == OP_STORE and words[i + 1] in chains:
            writes.add(words[i + 1])
        elif OP_ATOMIC_FIRST <= op <= OP_ATOMIC_LAST and words[i + 3] in chains:
            writes.add(words[i + 3])
    bool_t = next((t for t, (op, a, _) in m.types.items() if op == OP_TYPE_BOOL), None)
    bound = words[3]
    pre = []

    def new_id():
        nonlocal bound
        bound += 1
        return bound - 1

    if bool_t is None:
        bool_t = new_id()
        pre.append(_inst(OP_TYPE_BOOL, bool_t))
    ctab = {}

    def const(t, val):
        if (t, val) in ctab:
            return ctab[(t, val)]
        for c, (ct, a, _) in m.consts.items():
            if ct == t and a == [val]:
                ctab[(t, val)] = c
                return c
        c = new_id()
        pre.append(_inst(OP_CONSTANT, t, c, val))
        ctab[(t, val)] = c
        return c

    len_c = const(elem, dwords + 2)
    arr_t = new_id()
    pre.append(_inst(OP_TYPE_ARRAY, arr_t, elem, len_c))
    ptr_t = new_id()
    pre.append(_inst(OP_TYPE_POINTER, ptr_t, SC_WORKGROUP, arr_t))
    types_of = {}
    for i, op, n in insts(words):
        if i >= m.first_function() and n >= 3 and op not in (OP_STORE, OP_LABEL, 247, 249, 250, 251, 252, 253, 254, 255, 246):
            types_of[words[i + 2]] = words[i + 1]
    patch, ins = {}, {}
    ins[vpos] = pre
    patch[(vpos, 1)] = ptr_t
    for cid, cpos in chains.items():
        idx = words[cpos + 4]
        it = m.consts[idx][0] if idx in m.consts else types_of.get(idx)
        if it is None or m.types.get(it, (None,))[0] != OP_TYPE_INT:
            return words, {"applied": False, "why": "index type unknown"}
        lim_c = const(it, dwords)
        sink = const(it, dwords + (1 if cid in writes else 0))
        c, s = new_id(), new_id()
        ins.setdefault(cpos, []).append(_inst(OP_ULESS, bool_t, c, idx, lim_c) + _inst(OP_SELECT, it, s, c, idx, sink))
        patch[(cpos, 4)] = s
    out = _rebuild(words, set(), ins, patch)
    out[3] = bound
    return out, {"applied": True, "old_dwords": old_n, "dwords": dwords, "chains": len(chains), "writing_chains": len(writes)}
