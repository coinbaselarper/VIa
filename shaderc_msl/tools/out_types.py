"""Pixel-shader output-type variants (recipe pass `output_types`).

The translation writes every colour export as a vec4 float output (rdna2spirv.py `export`): an uncompressed export
stores the four export dwords bit-cast to float, a compressed ('compr') export the two f16 pairs of its two dwords
decoded with UnpackHalf2x16. Metal requires the fragment output's type class to match the attachment's (float /
uint / int), so a PSO that binds an integer render target to such an output is refused.

The console code in the corpus exports every colour through `v_cvt_pkrtz_f16_f32` + `exp ... compr`, also where
the HLSL signature (DXBC OSG1) declares the target uint: e.g. `&PS_HairStrandVBuffer` (uint2 to RG32_UINT)
exports `pkrtz(v2, v3)`, `(TL_PS_PBR_MATERIAL_*` target 1 (uint to R32_UINT) `pkrtz(v6, v6)`. So the binaries are
compiled for an FP16 export and the console binds the real export format with the PSO (inferred: the shader writes
uint registers and only the 16-bit float export would be lossy for them). A variant therefore exports the values
before the f16 packing:

- each compressed component whose pack is the translator's software pkrtz (or PackHalf2x16) in the same block as
  the export store takes that pack's 32-bit input instead ("recovered"); one that cannot be traced takes the raw f16
  bits of its half ("lossy", counted in the report);
- the float output moves to a Private shadow, and every return of the entry function writes an integer output of
  the same location from it by bit cast (as D3D does for an integer value: the dword as it is). A location the
  signature declares float keeps the same rule; D3D leaves a float written to an integer target undefined.

Variant codes are one character per colour location 0..n-1 (the PSO's render-target count): f (float, unorm, snorm
or no attachment: unchanged), u (uint), i (sint). Entry name `main0_o<code>`.
"""
import struct

import spv

OP_EXT_IMPORT, OP_EXT_INST, OP_BITCAST, OP_COMPOSITE_CONSTRUCT, OP_COMPOSITE_EXTRACT = 11, 12, 124, 80, 81
OP_VECTOR_SHUFFLE, OP_RETURN, OP_CONSTANT_NULL, OP_CONSTANT_COMPOSITE, OP_FUNCTION_END = 79, 253, 46, 44, 56
OP_SHR, OP_SHL, OP_OR, OP_AND, OP_MEMORY_MODEL = 194, 196, 197, 199, 14
DEC_INDEX = 32
GLSL_PACK_HALF, GLSL_UNPACK_HALF = 58, 62
VERSION = 2
CODES = set("fui")

INT_OPS = {126, 128, 130, 132, 134, 135, 137, 138, 139, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205,
           109, 110, 113, 114, 117, 4433, 4434, 4435, 4436}
FLOAT_OPS = {127, 129, 131, 133, 136, 140, 141, 142, 143, 148, 111, 112, 115, 116, 207, 208, 209, 210, 211, 212, 213,
             214, 215}
MEMORY_OPS = set(range(87, 99)) | {98, 227, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242}
NO_RESULT_TYPE = ({2, 3, 4, 5, 6, 7, 8, 10, 11, 14, 15, 16, 17, 62, 63, 64, 71, 72, 73, 74, 75, 99, 218, 219, 220, 221,
                   224, 225, 228, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255, 256, 257, 317, 330, 331, 332, 4416,
                   5380, 5294, 5295, 5632, 5633} | set(range(19, 40)))
INT_EXT = {GLSL_PACK_HALF, 54, 55, 56, 57, 59, 73, 74, 75, 39, 41, 42, 43}


def dxbc_targets(path):
    """{target register: 'uint'|'int'|'float'} from a DXBC container's OSG1/OSGN/OSG5 SV_Target entries, or None."""
    try:
        b = open(path, "rb").read()
    except OSError:
        return None
    i = b.find(b"DXBC")
    if i < 0:
        return None
    n = struct.unpack_from("<I", b, i + 28)[0]
    out = {}
    for o in struct.unpack_from("<%dI" % n, b, i + 32):
        tag, size = b[i + o:i + o + 4], struct.unpack_from("<I", b, i + o + 4)[0]
        if tag not in (b"OSGN", b"OSG1", b"OSG5"):
            continue
        d = b[i + o + 8:i + o + 8 + size]
        es = {b"OSGN": 24, b"OSG1": 32, b"OSG5": 28}[tag]
        skip = 0 if tag == b"OSGN" else 4
        for k in range(struct.unpack_from("<I", d, 0)[0]):
            name_off, _si, _sv, ct, reg = struct.unpack_from("<5I", d, 8 + k * es + skip)
            name = d[name_off:d.index(b"\0", name_off)].decode("ascii", "replace")
            if name.upper() == "SV_TARGET":
                out[reg] = {1: "uint", 2: "int", 3: "float"}.get(ct, "?")
    return out


def _block_index(words):
    """Instruction position -> id of the basic block it is in (None outside functions)."""
    blk, cur = {}, None
    for i, op, n in spv.insts(words):
        if op == spv.OP_LABEL:
            cur = words[i + 1]
        elif op == spv.OP_FUNCTION:
            cur = None
        blk[i] = cur
    return blk


class _Code:
    def __init__(self, words, m):
        self.w, self.m = words, m
        self.defs, self.stores, self.fn_vars = {}, {}, set()
        for i, op, n in spv.insts(words):
            if op == spv.OP_STORE:
                self.stores.setdefault(self.key(words[i + 1]), []).append((i, words[i + 2]))
            elif op == spv.OP_VARIABLE:
                if words[i + 3] == spv.SC_FUNCTION:
                    self.fn_vars.add(words[i + 2])
            elif n >= 3 and op not in NO_RESULT_TYPE:
                self.defs[words[i + 2]] = (i, op, n)
        self.block = _block_index(words)
        self.memo = {}

    def key(self, ptr):
        """A register (Function or Private variable, or a constant-index element of one) as a tuple; other
        pointers as themselves."""
        if ptr in self.fn_vars or (ptr in self.m.vars and self.m.vars[ptr][1] == spv.SC_PRIVATE):
            return (ptr,)
        e = self.defs.get(ptr)
        if e and e[1] in (spv.OP_ACCESS_CHAIN, spv.OP_IN_BOUNDS_ACCESS_CHAIN):
            w = self.w[e[0]:e[0] + e[2]]
            base = self.key(w[3])
            if isinstance(base, tuple):
                idx = []
                for x in w[4:]:
                    c = self.m.consts.get(x)
                    if c is None:
                        return ptr
                    idx.append(c[1][0])
                return base + tuple(idx)
        return ptr

    def reg(self, ptr):
        return isinstance(self.key(ptr), tuple)

    def last_store(self, var, before):
        best = None
        for pos, val in self.stores.get(self.key(var), []):
            if pos < before and (best is None or pos > best[0]):
                best = (pos, val)
        return best

    def op(self, vid):
        e = self.defs.get(vid)
        return (e[1], self.w[e[0]:e[0] + e[2]], e[0]) if e else (None, None, None)

    def through_regs(self, vid, before, var_self=None):
        """Follow loads of registers to the value stored last, and exec selects (Select c new old-of-same-reg) to the
        new value. Returns (id, position of its definition or store)."""
        for _ in range(8):
            op, w, pos = self.op(vid)
            if op == spv.OP_LOAD and self.reg(w[3]):
                st = self.last_store(w[3], pos)
                if st is None or self.block.get(st[0]) != self.block.get(pos):
                    return vid, pos
                sop, sw, _ = self.op(st[1])
                if sop == spv.OP_SELECT:
                    lo, _, _ = self.op(sw[5])
                    if lo == spv.OP_LOAD and self.key(self.op(sw[5])[1][3]) == self.key(w[3]):
                        vid = sw[4]
                        continue
                vid = st[1]
                continue
            return vid, pos
        return vid, None

    def classify(self, vid, depth=0):
        key = vid
        if key in self.memo:
            return self.memo[key]
        self.memo[key] = {"unknown"}
        r = self._classify(vid, depth)
        self.memo[key] = r
        return r

    def _classify(self, vid, depth):
        if depth > 32:
            return {"unknown"}
        op, w, pos = self.op(vid)
        if op is None:
            return {"unknown"}
        if op in (spv.OP_CONSTANT, OP_CONSTANT_NULL, OP_CONSTANT_COMPOSITE):
            return {"const"}
        if op in INT_OPS:
            return {"int"}
        if op in FLOAT_OPS:
            return {"float"}
        if op in MEMORY_OPS:
            return {"memory"}
        if op == OP_EXT_INST:
            if w[4] == GLSL_UNPACK_HALF:
                return {"f16pair"}
            return {"int"} if w[4] in INT_EXT else {"float"}
        if op == OP_BITCAST:
            return self.classify(w[3], depth + 1)
        if op == spv.OP_SELECT:
            return self.classify(w[4], depth + 1) | self.classify(w[5], depth + 1)
        if op == spv.OP_PHI:
            out = set()
            for k in range(3, len(w), 2):
                out |= self.classify(w[k], depth + 1)
            return out
        if op in (OP_COMPOSITE_EXTRACT, OP_VECTOR_SHUFFLE):
            return self.classify(w[3], depth + 1)
        if op == OP_COMPOSITE_CONSTRUCT:
            out = set()
            for k in range(3, len(w)):
                out |= self.classify(w[k], depth + 1)
            return out
        if op == spv.OP_LOAD:
            if self.reg(w[3]):
                st = self.last_store(w[3], pos)
                return self.classify(st[1], depth + 1) if st else {"const"}
            return {"memory"}
        return {"unknown"}

    def pk_inputs(self, dword, use_pos):
        """The two 32-bit inputs (lo, hi) of the f16 pack that made `dword`, as ('u', uint id) or ('f', float id),
        when they are defined in the block of use_pos before it; else None."""
        v, _ = self.through_regs(dword, use_pos)
        return self.pack_parts(v, use_pos)

    def pack_parts(self, v, use_pos):
        op, w, pos = self.op(v)
        blk = self.block.get(use_pos)

        def ok(x):
            e = self.defs.get(x)
            if e is None:
                return x in self.m.consts
            return self.block.get(e[0]) == blk and e[0] < use_pos

        if op == OP_EXT_INST and w[4] == GLSL_PACK_HALF:
            cop, cw, _ = self.op(w[5])
            if cop == OP_COMPOSITE_CONSTRUCT and len(cw) == 5 and ok(cw[3]) and ok(cw[4]):
                return ("f", cw[3]), ("f", cw[4])
            return None
        if op != OP_OR:
            return None
        lo = hi = None
        for a, b in ((w[3], w[4]), (w[4], w[3])):
            bop, bw, _ = self.op(b)
            if bop == OP_SHL and self.m.consts.get(bw[4], (None, [None]))[1] == [16]:
                lo, hi = a, bw[3]
                break
        if lo is None:
            return None
        ins = []
        for h in (lo, hi):
            src = self.f16_source(h)
            if src is None or not ok(src):
                return None
            ins.append(("u", src))
        return tuple(ins)

    def reg_of_load(self, vid):
        """The register key a dword was loaded from, when every pointer into that register's variable has constant
        indices (so every store to it is known); else None."""
        op, w, _ = self.op(vid)
        if op != spv.OP_LOAD:
            return None
        k = self.key(w[3])
        if not isinstance(k, tuple) or k[0] in self.dynamic_bases():
            return None
        return k

    def dynamic_bases(self):
        if not hasattr(self, "_dyn"):
            self._dyn = set()
            for vid, (i, op, n) in self.defs.items():
                if op in (spv.OP_ACCESS_CHAIN, spv.OP_IN_BOUNDS_ACCESS_CHAIN) and not isinstance(self.key(vid), tuple):
                    b = self.key(self.w[i + 3])
                    if isinstance(b, tuple):
                        self._dyn.add(b[0])
        return self._dyn

    def reg_stores(self, k):
        """[(store pos, exec condition or None, packed parts or None, value)] for every store to register k."""
        out = []
        for pos, val in self.stores.get(k, []):
            cond, new = None, val
            op, w, _ = self.op(val)
            if op == spv.OP_SELECT:
                lop, lw, _ = self.op(w[5])
                if lop == spv.OP_LOAD and self.key(lw[3]) == k:
                    cond, new = w[3], w[4]
            out.append((pos, cond, self.pack_parts(new, pos), val))
        return out

    def f16_source(self, h):
        """The translator's software f32->f16: (... | sign) with sign = (A >> 16) & 0x8000; returns A."""
        op, w, _ = self.op(h)
        if op != OP_OR:
            return None
        for x in (w[3], w[4]):
            xop, xw, _ = self.op(x)
            if xop == OP_AND and self.m.consts.get(xw[4], (None, [None]))[1] == [0x8000]:
                sop, sw, _ = self.op(xw[3])
                if sop == OP_SHR and self.m.consts.get(sw[4], (None, [None]))[1] == [16]:
                    return sw[3]
        return None


def colour_outputs(m):
    """{location: (var id, component type id, components, float?)} of the fragment shader's colour outputs."""
    out = {}
    for d in m.interface():
        if d["sc"] != "out" or d["builtin"] is not None or d["location"] is None:
            continue
        if m.dec(d["id"], DEC_INDEX) not in (None, 0):
            continue
        t = m.pointee(d["id"])
        op, a, _ = m.types[t]
        if op == spv.OP_TYPE_VECTOR:
            comp, n = a[0], a[1]
        elif op in (spv.OP_TYPE_FLOAT, spv.OP_TYPE_INT):
            comp, n = t, 1
        else:
            continue
        out[d["location"]] = (d["id"], comp, n, m.types[comp][0] == spv.OP_TYPE_FLOAT)
    return out


def _is_load_of(code, vid, var):
    op, w, _ = code.op(vid)
    return op == spv.OP_LOAD and w[3] == var


def export_sites(code, var):
    """Stores of new values to output `var`: [(store pos, construct id or None, [(slot, component id)])]; slots
    that read the output back (kept from an earlier export) are left out."""
    sites = []
    for pos, val in code.stores.get(var, []):
        op, w, _ = code.op(val)
        if op == spv.OP_SELECT:
            if _is_load_of(code, w[5], var):
                val = w[4]
            elif _is_load_of(code, w[4], var):
                val = w[5]
        op, w, _ = code.op(val)
        if op == OP_COMPOSITE_CONSTRUCT:
            comps = []
            for k, c in enumerate(w[3:]):
                cop, cw, _ = code.op(c)
                if cop == OP_COMPOSITE_EXTRACT and _is_load_of(code, cw[3], var):
                    continue
                comps.append((k, c))
            sites.append((pos, val, comps))
        else:
            sites.append((pos, None, [(None, val)]))
    return sites


def _unpack_of(code, comp):
    """(dword id, half index) when comp = CompositeExtract(UnpackHalf2x16(dword), j)."""
    op, w, _ = code.op(comp)
    if op != OP_COMPOSITE_EXTRACT or len(w) != 5:
        return None
    uop, uw, _ = code.op(w[3])
    if uop == OP_EXT_INST and uw[4] == GLSL_UNPACK_HALF:
        return uw[5], w[4]
    return None


def analyze(words, signature=None):
    """Per colour location: type (SPIR-V), declared (DXBC signature type or None), components, compressed
    (True/False/None when mixed), sources (classes of the exported values), recoverable/lossy component counts."""
    m = spv.Module(words)
    if m.model != "ps":
        return {}
    code = _Code(words, m)
    res = {}
    for loc, (var, comp, n, isf) in colour_outputs(m).items():
        r = {"type": "float" if isf else "int", "declared": (signature or {}).get(loc), "components": n,
             "stores": 0, "recovered": 0, "lossy": 0}
        kinds, sources = set(), set()
        for pos, cons, comps in export_sites(code, var):
            r["stores"] += 1
            for slot, c in comps:
                up = _unpack_of(code, c)
                if up is None:
                    kinds.add("plain")
                    sources |= code.classify(c)
                    continue
                kinds.add("compressed")
                pk = code.pk_inputs(up[0], pos if cons is None else code.defs[cons][0])
                if pk is not None:
                    r["recovered"] += 1
                    kind, src = pk[up[1]]
                    sources |= {("pk:" + s) for s in code.classify(src)}
                    continue
                k = code.reg_of_load(up[0])
                st = code.reg_stores(k) if k else []
                lpos = code.defs[up[0]][0]
                last = max((x for x in st if x[0] < lpos), default=None)
                if last is not None and last[2] is not None:
                    r["recovered"] += 1
                    r["shadowed"] = r.get("shadowed", 0) + 1
                    sources |= {("pk:" + s) for s in code.classify(last[2][up[1]][1])}
                else:
                    r["lossy"] += 1
                    sources.add("f16")
        r["compressed"] = None if len(kinds) > 1 else ("compressed" in kinds)
        r["sources"] = sorted(sources)
        res[loc] = r
    return res


def normalize(code):
    code = code.strip()
    if not code or set(code) - CODES:
        raise ValueError("output variant code %r: one of f/u/i per colour location" % code)
    return code


def needs_variant(info, code):
    return any(c != "f" and info.get(loc, {}).get("type") == "float" for loc, c in enumerate(code))


def output_types(words, code, info=None):
    """The variant for `code`: returns (words, report); words unchanged (report["applied"] False) when no location
    needs one."""
    code = normalize(code)
    m = spv.Module(words)
    if m.model != "ps":
        return words, {"applied": False, "why": "not a pixel shader"}
    outs = colour_outputs(m)
    plan = {loc: c for loc, c in enumerate(code) if c != "f" and loc in outs and outs[loc][3]}
    if not plan:
        return words, {"applied": False, "why": "no float output at an integer location"}
    cd = _Code(words, m)
    bound = words[3]

    def new_id():
        nonlocal bound
        bound += 1
        return bound - 1

    pre, early = [], []

    def find_type(op, args, first=False):
        for t, (o, a, _) in m.types.items():
            if o == op and a == args:
                return t
        t = new_id()
        (early if first else pre).append(spv._inst(op, t, *args))
        m.types[t] = (op, args, None)
        return t

    def const(t, val):
        for c, (ct, a, _) in m.consts.items():
            if ct == t and a == [val]:
                return c
        c = new_id()
        pre.append(spv._inst(spv.OP_CONSTANT, t, c, val))
        m.consts[c] = (t, [val], None)
        return c

    float_t = find_type(spv.OP_TYPE_FLOAT, [32])
    uint_t = find_type(spv.OP_TYPE_INT, [32, 0])
    int_t = find_type(spv.OP_TYPE_INT, [32, 1])
    v2f = find_type(spv.OP_TYPE_VECTOR, [float_t, 2])
    glsl, mm_pos = None, None
    for i, op, n in spv.insts(words):
        if op == OP_EXT_IMPORT and spv.lit_str(words, i + 2) == "GLSL.std.450":
            glsl = words[i + 1]
        if op == OP_MEMORY_MODEL and mm_pos is None:
            mm_pos = i
    ins, patch = {}, {}
    if glsl is None:
        glsl = new_id()
        name = b"GLSL.std.450\0\0\0\0"
        ins.setdefault(mm_pos, []).append(spv._inst(OP_EXT_IMPORT, glsl, *[int.from_bytes(name[k:k + 4], "little")
                                                                           for k in range(0, 16, 4)]))
    report = {"applied": True, "code": code, "version": VERSION, "locations": {}}
    work = []
    shadows, fn_vars = {}, []
    p_fn_uint = find_type(spv.OP_TYPE_POINTER, [spv.SC_FUNCTION, uint_t])

    def shadow(k):
        """Function-scope copies of register k's packed inputs, written beside every store to k: the pack's
        32-bit inputs, or for a store of anything else the raw f16 halves of the stored dword."""
        if k in shadows:
            return shadows[k]
        lo, hi = new_id(), new_id()
        fn_vars.append(spv._inst(spv.OP_VARIABLE, p_fn_uint, lo, spv.SC_FUNCTION))
        fn_vars.append(spv._inst(spv.OP_VARIABLE, p_fn_uint, hi, spv.SC_FUNCTION))
        shadows[k] = (lo, hi)
        lossy_stores = 0
        for pos, cond, parts, val in cd.reg_stores(k):
            body = []
            if parts is None:
                lossy_stores += cd.op(val)[0] not in (spv.OP_CONSTANT, OP_CONSTANT_NULL)
                a, b = new_id(), new_id()
                body += spv._inst(OP_AND, uint_t, a, val, const(uint_t, 0xFFFF))
                body += spv._inst(OP_SHR, uint_t, b, val, const(uint_t, 16))
                vals = (a, b)
            else:
                vals = []
                for kind, src in parts:
                    if kind == "u":
                        vals.append(src)
                    else:
                        u = new_id()
                        body += spv._inst(OP_BITCAST, uint_t, u, src)
                        vals.append(u)
            for var, x in zip((lo, hi), vals):
                if cond is not None:
                    old, sel = new_id(), new_id()
                    body += spv._inst(spv.OP_LOAD, uint_t, old, var)
                    body += spv._inst(spv.OP_SELECT, uint_t, sel, cond, x, old)
                    x = sel
                body += spv._inst(spv.OP_STORE, var, x)
            ins.setdefault(pos, []).append(body)
        report.setdefault("shadow_other_stores", 0)
        report["shadow_other_stores"] += lossy_stores
        return shadows[k]

    for loc, c in sorted(plan.items()):
        var, comp, n, _f = outs[loc]
        _ptype, _sc, vpos = m.vars[var]
        pointee = m.pointee(var)
        signed = c == "i"
        it = int_t if signed else uint_t
        out_t = find_type(spv.OP_TYPE_VECTOR, [it, n]) if n > 1 else it
        p_out = find_type(spv.OP_TYPE_POINTER, [spv.SC_OUTPUT, out_t])
        p_priv = find_type(spv.OP_TYPE_POINTER, [spv.SC_PRIVATE, pointee], first=True)
        nv = new_id()
        patch[(vpos, 1)] = p_priv
        patch[(vpos, 3)] = spv.SC_PRIVATE
        for k, args, dpos in m.decor.get(var, []):
            if k in (spv.DEC_LOCATION, DEC_INDEX, spv.DEC_COMPONENT):
                patch[(dpos, 1)] = nv
        rec = lossy = 0
        for pos, cons, comps in export_sites(cd, var):
            if cons is None:
                continue
            cpos = cd.defs[cons][0]
            for slot, cid in comps:
                up = _unpack_of(cd, cid)
                if up is None:
                    continue
                pk = cd.pk_inputs(up[0], cpos)
                nid = new_id()
                k = cd.reg_of_load(up[0]) if pk is None else None
                if k is not None:
                    lo_hi = shadow(k)
                    ins.setdefault(cpos, []).append(spv._inst(spv.OP_LOAD, uint_t, nid, lo_hi[up[1]]))
                    fid = new_id()
                    ins[cpos].append(spv._inst(OP_BITCAST, float_t, fid, nid))
                    patch[(cpos, 3 + slot)] = fid
                    rec += 1
                    continue
                if pk is not None:
                    kind, src = pk[up[1]]
                    if kind == "u":
                        ins.setdefault(cpos, []).append(spv._inst(OP_BITCAST, float_t, nid, src))
                    else:
                        ins.setdefault(cpos, []).append(spv._inst(spv.OP_COPY_OBJECT, float_t, nid, src))
                    rec += 1
                else:
                    pv, p, a = new_id(), new_id(), new_id()
                    ins.setdefault(cpos, []).append(
                        spv._inst(OP_COMPOSITE_CONSTRUCT, v2f, pv, cid, const(float_t, 0))
                        + spv._inst(OP_EXT_INST, uint_t, p, glsl, GLSL_PACK_HALF, pv)
                        + spv._inst(OP_AND, uint_t, a, p, const(uint_t, 0xFFFF))
                        + spv._inst(OP_BITCAST, float_t, nid, a))
                    lossy += 1
                patch[(cpos, 3 + slot)] = nid
        work.append((var, pointee, out_t, nv, p_out))
        report["locations"][str(loc)] = {"signed": signed, "recovered": rec, "lossy": lossy}

    priv_chain = {}
    for i, op, n in spv.insts(words):
        if op in (spv.OP_ACCESS_CHAIN, spv.OP_IN_BOUNDS_ACCESS_CHAIN) and any(words[i + 3] == x[0] for x in work):
            rt = words[i + 1]
            if rt not in priv_chain:
                priv_chain[rt] = find_type(spv.OP_TYPE_POINTER, [spv.SC_PRIVATE, m.types[rt][1][1]])
            patch[(i, 1)] = priv_chain[rt]

    def epilogue():
        body = []
        for var, pointee, out_t, nv, _p in work:
            v, u = new_id(), new_id()
            body += spv._inst(spv.OP_LOAD, pointee, v, var)
            body += spv._inst(OP_BITCAST, out_t, u, v)
            body += spv._inst(spv.OP_STORE, nv, u)
        return body

    in_entry, returns = False, 0
    for i, op, n in spv.insts(words):
        if op == spv.OP_FUNCTION:
            in_entry = words[i + 2] == m.entry["fn"]
        elif op == OP_FUNCTION_END:
            in_entry = False
        elif in_entry and op == OP_RETURN:
            ins.setdefault(i, []).append(epilogue())
            returns += 1
    if returns == 0:
        raise ValueError("output_types: the entry function has no OpReturn")
    if fn_vars:
        in_entry, label_seen = False, False
        for i, op, n in spv.insts(words):
            if op == spv.OP_FUNCTION:
                in_entry = words[i + 2] == m.entry["fn"]
            elif in_entry and op == spv.OP_LABEL and not label_seen:
                label_seen = True
            elif in_entry and label_seen and op != spv.OP_VARIABLE:
                ins.setdefault(i, []).insert(0, sum(fn_vars, []))
                break
    first_var = min(m.vars[x[0]][2] for x in work)
    ins.setdefault(first_var, []).insert(0, sum(early, []))
    new_vars = sum((spv._inst(spv.OP_VARIABLE, p, nv, spv.SC_OUTPUT) for _v, _pt, _o, nv, p in work), [])
    ins.setdefault(m.first_function(), []).insert(0, sum(pre, []) + new_vars)
    out = spv._rebuild(words, (), ins, patch)
    for i, op, n in spv.insts(out):
        if op == spv.OP_ENTRY:
            if words[1] >= 0x10400:
                hdr = out[i:i + n] + [x[3] for x in work]
            else:
                swap = {x[0]: x[3] for x in work}
                hdr = [swap.get(x, x) if k >= 3 else x for k, x in enumerate(out[i:i + n])]
            hdr[0] = (len(hdr) << 16) | spv.OP_ENTRY
            out = out[:i] + hdr + out[i + n:]
            break
    out[3] = bound
    report["returns"] = returns
    return out, report


def describe(info):
    """One line per location for the build log."""
    parts = []
    for loc in sorted(info):
        r = info[loc]
        kind = "compressed" if r["compressed"] else ("mixed" if r["compressed"] is None else "plain")
        parts.append("%d:%s(decl %s)/%s rec %d lossy %d/%s" % (loc, r["type"], r["declared"] or "?", kind,
                                                            r["recovered"], r["lossy"], "+".join(r["sources"]) or "-"))
    return " ".join(parts)
