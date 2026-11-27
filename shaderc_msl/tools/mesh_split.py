#!/usr/bin/env python3
"""Meshlet split as a SPIR-V transform: one mesh shader of up to 128 vertices -> an object (task) stage plus a mesh
stage that emits at most vmax vertices and pmax primitives per mesh threadgroup.

The translator's mesh shaders compute every output in per-lane registers (lane t holds vertex t and primitive t), call
OpSetMeshOutputsEXT(V, P) after a barrier and then store them at index t. The transform keeps the whole program and
rewrites only that epilogue:

  object stage   one thread per original group; launches `chunks` mesh groups and passes the original WorkgroupId and
                 NumWorkgroups through a TaskPayloadWorkgroupEXT variable
  mesh stage     WorkgroupId / NumWorkgroups come from the payload (the program is unchanged); the real WorkgroupId.x is
                 the chunk number c. Lane 0 walks the P primitives in order and cuts a new chunk when the next primitive
                 would take the current one past vmax distinct vertices or pmax primitives (tags per vertex in a
                 workgroup array). Chunk c emits the primitives of its range and the vertices they use, renumbered
                 into 0..vmax-1 in first-use order; a vertex used by two chunks is emitted by both (its lane stores
                 it in each chunk's group, because every group runs the whole program).
  capture        optional: every output store is mirrored into a storage buffer (descriptor set 31, binding 0) for the
                 output-comparison test, plus the chunk's header (counts, chunks needed).

Every chunk group runs the whole program, so the mesh stage costs `chunks` times the original plus lane 0's walk.
Primitives past the last chunk are not emitted; `chunks_needed` (the walk's final chunk number) is what capture
records, and with chunks = 7 (the bound for vmax 64, pmax 64) no input can overflow.

Requires: the entry function holds the single OpSetMeshOutputsEXT, local size x is 128, the epilogue reads the lane
index from LocalInvocationIndex and stores the per-vertex outputs and PrimitiveTriangleIndicesEXT / CullPrimitiveEXT
at that index. Anything else raises SplitError.
"""
import os, re, subprocess, sys, tempfile

SPIRV_AS = os.environ.get("SHADERC_SPIRV_AS", "/opt/homebrew/bin/spirv-as")
SPIRV_DIS = os.environ.get("SHADERC_SPIRV_DIS", "/opt/homebrew/bin/spirv-dis")
SPIRV_VAL = os.environ.get("SHADERC_SPIRV_VAL", "/opt/homebrew/bin/spirv-val")
VERSION = 1
CAPTURE_SET = 30
ID_RE = re.compile(r"%[A-Za-z0-9_]+")
DEF_RE = re.compile(r"^\s*(%[A-Za-z0-9_]+) = (Op\w+)")


class SplitError(Exception):
    pass


def disassemble(words):
    import struct
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "m.spv")
        open(p, "wb").write(struct.pack("<%dI" % len(words), *words))
        r = subprocess.run([SPIRV_DIS, "--raw-id", "--no-header", p], capture_output=True, text=True)
    if r.returncode:
        raise SplitError("spirv-dis: " + r.stderr[:300])
    return r.stdout


def assemble(text, target="spv1.5"):
    import struct
    with tempfile.TemporaryDirectory() as d:
        p, o = os.path.join(d, "m.spvasm"), os.path.join(d, "m.spv")
        open(p, "w").write(text)
        r = subprocess.run([SPIRV_AS, "--target-env", target, p, "-o", o], capture_output=True, text=True)
        if r.returncode:
            raise SplitError("spirv-as: " + r.stderr[:600])
        raw = open(o, "rb").read()
    return list(struct.unpack("<%dI" % (len(raw) // 4), raw))


def validate(words, env="vulkan1.3"):
    import struct
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "m.spv")
        open(p, "wb").write(struct.pack("<%dI" % len(words), *words))
        r = subprocess.run([SPIRV_VAL, "--target-env", env, p], capture_output=True, text=True)
    return r.returncode == 0, r.stderr.strip()[:600]


def task_module(chunks):
    t = f"""OpCapability Shader
OpCapability MeshShadingEXT
OpExtension "SPV_EXT_mesh_shader"
OpMemoryModel Logical GLSL450
OpEntryPoint TaskEXT %main "main" %wgid %nwg %payload
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %wgid BuiltIn WorkgroupId
OpDecorate %nwg BuiltIn NumWorkgroups
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%v3uint = OpTypeVector %uint 3
%pay_t = OpTypeStruct %v3uint %v3uint
%p_in = OpTypePointer Input %v3uint
%p_pay = OpTypePointer TaskPayloadWorkgroupEXT %pay_t
%p_pay_v3 = OpTypePointer TaskPayloadWorkgroupEXT %v3uint
%c0 = OpConstant %uint 0
%c1 = OpConstant %uint 1
%ck = OpConstant %uint {chunks}
%wgid = OpVariable %p_in Input
%nwg = OpVariable %p_in Input
%payload = OpVariable %p_pay TaskPayloadWorkgroupEXT
%main = OpFunction %void None %fn
%entry = OpLabel
%w = OpLoad %v3uint %wgid
%n = OpLoad %v3uint %nwg
%pw = OpAccessChain %p_pay_v3 %payload %c0
%pn = OpAccessChain %p_pay_v3 %payload %c1
OpStore %pw %w
OpStore %pn %n
OpEmitMeshTasksEXT %ck %c1 %c1 %payload
OpFunctionEnd
"""
    return t


def hoist_types(lines):
    """Types and constants before variables in the global section (an earlier pass may leave a variable ahead of a type
    it uses); the relative order inside each group is kept."""
    first = next((i for i, l in enumerate(lines) if re.match(r"\s*%\w+ = (OpType|OpConstant|OpSpecConstant|OpVariable|OpUndef)", l)), None)
    last = next((i for i, l in enumerate(lines) if re.match(r"\s*%\w+ = OpFunction", l)), None)
    if first is None or last is None:
        return lines
    seg = lines[first:last]
    if any(not re.match(r"\s*(%\w+ = (OpType|OpConstant|OpSpecConstant|OpVariable|OpUndef)|$)", l) for l in seg):
        return lines
    vars_ = [l for l in seg if "= OpVariable" in l]
    rest = [l for l in seg if "= OpVariable" not in l]
    return lines[:first] + rest + vars_ + lines[last:]


def find(lines, pat, start=0, end=None):
    rx = re.compile(pat)
    for i in range(start, end if end is not None else len(lines)):
        if rx.search(lines[i]):
            return i
    return -1


def split_mesh(words, chunks=3, vmax=64, pmax=64, capture=False, mutant=False):
    """Returns (mesh_words, task_words, report)."""
    text = disassemble(words)
    lines = hoist_types(text.split("\n"))
    n = len(lines)

    def one(pat, what):
        hits = [i for i in range(n) if re.search(pat, lines[i])]
        if len(hits) != 1:
            raise SplitError(f"{what}: {len(hits)} matches")
        return hits[0]

    ep = one(r"OpEntryPoint MeshEXT ", "mesh entry point")
    m = re.match(r"\s*OpEntryPoint MeshEXT (%\w+) \"(\w+)\"((?: %\w+)*)", lines[ep])
    fn, ename, iface = m.group(1), m.group(2), m.group(3)
    if re.search(r"OpExecutionMode %\w+ LocalSize (?!128 1 1)", text):
        raise SplitError("local size is not 128 1 1")
    if "OutputTrianglesEXT" not in text:
        raise SplitError("not a triangle mesh shader")
    em_v = one(r"OpExecutionMode %\w+ OutputVertices \d+", "OutputVertices")
    em_p = one(r"OpExecutionMode %\w+ OutputPrimitivesEXT \d+", "OutputPrimitivesEXT")

    builtin = {}
    for i in range(n):
        mm = re.match(r"\s*OpDecorate (%\w+) BuiltIn (\w+)", lines[i])
        if mm:
            builtin[mm.group(2)] = mm.group(1)
    for need in ("WorkgroupId", "LocalInvocationIndex", "PrimitiveTriangleIndicesEXT"):
        if need not in builtin:
            raise SplitError("no BuiltIn " + need)
    add_nwg = "NumWorkgroups" not in builtin
    wgid_v, nwg_v, lid_v, idx_v = (builtin["WorkgroupId"], builtin.get("NumWorkgroups", "%sp_nwv"), builtin["LocalInvocationIndex"],
                                   builtin["PrimitiveTriangleIndicesEXT"])

    types = {}
    for i in range(n):
        mm = re.match(r"\s*(%\w+) = (OpType\w+)(?: (.*))?$", lines[i])
        if mm:
            types[mm.group(1)] = (mm.group(2), (mm.group(3) or "").split(), i)
    uint = next((k for k, (o, a, _) in types.items() if o == "OpTypeInt" and a == ["32", "0"]), None)
    boolt = next((k for k, (o, a, _) in types.items() if o == "OpTypeBool"), None)
    v3uint = next((k for k, (o, a, _) in types.items() if o == "OpTypeVector" and a == [uint, "3"]), None)
    ptr_wg_uint = next((k for k, (o, a, _) in types.items() if o == "OpTypePointer" and a == ["Workgroup", uint]), None)
    if None in (uint, boolt, v3uint):
        raise SplitError("missing uint / bool / v3uint types")

    out_var_new = {}
    out_vars, prim_struct_vars = {}, set()
    for i in range(n):
        mm = re.match(r"\s*(%\w+) = OpVariable (%\w+) (Output|Workgroup|Private)\b", lines[i])
        if mm and mm.group(3) == "Output":
            out_vars[mm.group(1)] = mm.group(2)
    per_prim_struct = set()
    for i in range(n):
        mm = re.match(r"\s*OpMemberDecorate (%\w+) \d+ PerPrimitiveEXT", lines[i])
        if mm:
            per_prim_struct.add(mm.group(1))
    per_prim_var = set()
    for i in range(n):
        mm = re.match(r"\s*OpDecorate (%\w+) PerPrimitiveEXT", lines[i])
        if mm:
            per_prim_var.add(mm.group(1))
    for v, pt in out_vars.items():
        t = types[pt]
        elem = types[t[1][1]]
        if elem[0] == "OpTypeArray":
            el = elem[1][0]
            if el in per_prim_struct:
                prim_struct_vars.add(v)
    prim_vars = {idx_v} | prim_struct_vars | (per_prim_var & set(out_vars))
    vert_vars = [v for v in out_vars if v not in prim_vars]

    fstart = one(r"^\s*" + re.escape(fn) + r" = OpFunction", "entry function")
    fend = find(lines, r"^\s*OpFunctionEnd", fstart)
    smo = [i for i in range(n) if "OpSetMeshOutputsEXT" in lines[i]]
    if len(smo) != 1 or not (fstart < smo[0] < fend):
        raise SplitError("OpSetMeshOutputsEXT is not unique or not in the entry function")
    smo = smo[0]
    V, P = re.match(r"\s*OpSetMeshOutputsEXT (%\w+) (%\w+)", lines[smo]).groups()
    first_fn = find(lines, r"^\s*%\w+ = OpFunction")

    L_ids = set()
    region_defs = {}
    for i in range(fstart, fend):
        mm = DEF_RE.match(lines[i])
        if mm and mm.group(2) == "OpLoad" and re.search(r"OpLoad " + re.escape(uint) + " " + re.escape(lid_v) + r"\b", lines[i]):
            L_ids.add(mm.group(1))
    for i in range(smo + 1, fend):
        mm = DEF_RE.match(lines[i])
        if mm:
            region_defs[mm.group(1)] = i
    if not L_ids:
        raise SplitError("the epilogue does not load LocalInvocationIndex")

    capture_after = {}
    ac_rest = {}
    prim_ac, vert_ac = {}, {}
    cond_v, cond_p = [], []
    new_region = {}
    idx_store = None
    for i in range(smo + 1, fend):
        ln = lines[i]
        mm = re.match(r"\s*(%\w+) = OpAccessChain (%\w+) (%\w+) (%\w+)(.*)$", ln)
        if mm and mm.group(3) in out_vars:
            r, pt, var, ix, rest = mm.groups()
            if ix not in L_ids:
                raise SplitError("output access chain not indexed by the lane index: " + ln.strip())
            target = "%sp_ps" if var in prim_vars else "%sp_vs"
            (prim_ac if var in prim_vars else vert_ac)[r] = var
            ac_rest[r] = (pt, rest.strip())
            new_region[i] = f"{r} = OpAccessChain {pt} {var} {target}{rest}"
            continue
        mm = re.match(r"\s*(%\w+) = OpULessThan " + re.escape(boolt) + r" (%\w+) (%\w+)$", ln)
        if mm and mm.group(2) in L_ids and mm.group(3) in (V, P):
            cond = "%sp_vok" if mm.group(3) == V else "%sp_pok"
            (cond_v if mm.group(3) == V else cond_p).append((i, mm.group(1)))
            new_region[i] = f"{mm.group(1)} = OpCopyObject {boolt} {cond}"
            continue
        mm = re.match(r"\s*OpStore (%\w+) (%\w+)(.*)$", ln)
        if mm and capture and mm.group(1) in vert_ac:
            capture_after.setdefault(i, []).append(("v", mm.group(1), mm.group(2), vert_ac[mm.group(1)], ac_rest[mm.group(1)]))
        if mm and capture and mm.group(1) in prim_ac and prim_ac[mm.group(1)] != idx_v \
                and types[types[ac_rest[mm.group(1)][0]][1][1]][0] == "OpTypeBool":
            capture_after.setdefault(i, []).append(("c", mm.group(1), mm.group(2), prim_ac[mm.group(1)], ac_rest[mm.group(1)]))
        if mm and mm.group(1) in prim_ac and prim_ac[mm.group(1)] == idx_v:
            if idx_store is not None:
                raise SplitError("more than one store to PrimitiveTriangleIndicesEXT")
            idx_store = (i, mm.group(2))
            new_region[i] = f"OpStore {mm.group(1)} %sp_ntri{mm.group(3)}"
    if idx_store is None or not cond_p or not cond_v:
        raise SplitError("epilogue pattern not found (idx store %s, prim cond %d, vertex cond %d)"
                         % (idx_store is not None, len(cond_p), len(cond_v)))
    for v in vert_vars:
        pass

    def cone(root, lo, hi):
        """Lines (in order) defining `root` and everything it needs that is defined in the epilogue region."""
        need, stack = set(), [root]
        while stack:
            x = stack.pop()
            if x in need or x not in region_defs:
                continue
            need.add(x)
            ln = lines[region_defs[x]]
            op = DEF_RE.match(ln).group(2)
            if op in ("OpPhi", "OpFunctionCall", "OpLabel") or op.startswith("OpAtomic"):
                raise SplitError(f"cone of {root} contains {op}")
            rhs = ln.split("=", 1)[1]
            for t in ID_RE.findall(rhs):
                if t in region_defs:
                    stack.append(t)
        order = sorted(need, key=lambda k: region_defs[k])
        for x in order:
            ln = lines[region_defs[x]]
            if re.search(r"OpLoad \S+ (%\w+)", ln):
                pass
        return order

    def dup(order, suffix):
        mp = {x: x + suffix for x in order}
        out = []
        for x in order:
            ln = lines[region_defs[x]]
            lhs, rhs = ln.split("=", 1)
            rhs = ID_RE.sub(lambda m_: mp.get(m_.group(0), m_.group(0)), rhs)
            out.append("%s = %s" % (mp[x], rhs.strip()))
        return out, mp

    val_root = idx_store[1]
    tri_order = cone(val_root, smo, fend)
    tri_lines, tri_map = dup(tri_order, "_sp1")
    tri_val = tri_map.get(val_root, val_root)
    pcond_id = cond_p[0][1]
    A = None
    for i in range(smo + 1, fend):
        mm = re.match(r"\s*(%\w+) = OpLogicalAnd " + re.escape(boolt) + r" (%\w+) (%\w+)$", lines[i])
        if mm and pcond_id in (mm.group(2), mm.group(3)):
            A = mm.group(3) if mm.group(2) == pcond_id else mm.group(2)
            break
    if A is None:
        a_lines, a_val = [], None
    else:
        a_order = cone(A, smo, fend)
        a_lines, a_map = dup(a_order, "_sp2")
        a_val = a_map.get(A, A)

    # ---- new global declarations
    consts = {}

    def c(v):
        consts[v] = f"%sp_c{v}"
        return consts[v]

    decl = []
    if ptr_wg_uint is None:
        ptr_wg_uint = "%sp_pwu"
        decl.append(f"{ptr_wg_uint} = OpTypePointer Workgroup {uint}")
    decl += [f"%sp_true = OpConstantTrue {boolt}", f"%sp_false = OpConstantFalse {boolt}",
             f"%sp_arr = OpTypeArray {uint} {c(128)}",
             "%sp_parr = OpTypePointer Workgroup %sp_arr",
             f"%sp_pay_t = OpTypeStruct {v3uint} {v3uint}",
             "%sp_ppay = OpTypePointer TaskPayloadWorkgroupEXT %sp_pay_t",
             f"%sp_ppay_v3 = OpTypePointer TaskPayloadWorkgroupEXT {v3uint}",
             f"%sp_ppay_u = OpTypePointer TaskPayloadWorkgroupEXT {uint}",
             "%sp_vt = OpVariable %sp_parr Workgroup", "%sp_pk = OpVariable %sp_parr Workgroup",
             "%sp_pn = OpVariable %sp_parr Workgroup", "%sp_cnt = OpVariable %sp_parr Workgroup",
             "%sp_payload = OpVariable %sp_ppay TaskPayloadWorkgroupEXT"]
    new_ifaces = ["%sp_payload", "%sp_vt", "%sp_pk", "%sp_pn", "%sp_cnt"]
    if add_nwg:
        pin = next((k for k, (o_, a_, _) in types.items() if o_ == "OpTypePointer" and a_ == ["Input", v3uint]), None)
        if pin is None:
            pin = "%sp_pinv3"
            decl.append(f"{pin} = OpTypePointer Input {v3uint}")
        decl.append(f"%sp_nwv = OpVariable {pin} Input")
        new_ifaces.append("%sp_nwv")

    cap = []
    cap_vars = []
    if capture:
        cap_vars = [idx_v] + [v for v in out_vars if v != idx_v]
        decl += [f"%sp_cap_rt = OpTypeRuntimeArray {uint}", "%sp_cap_st = OpTypeStruct %sp_cap_rt",
                 "%sp_cap_pt = OpTypePointer StorageBuffer %sp_cap_st", f"%sp_cap_pu = OpTypePointer StorageBuffer {uint}",
                 "%sp_cap = OpVariable %sp_cap_pt StorageBuffer"]
        new_ifaces.append("%sp_cap")

    retype, retype_decl, retype_first = {}, {}, {}
    for v, pt in out_vars.items():
        want = pmax if v in prim_vars else vmax
        arr = types[pt][1][1]
        key = (arr, want)
        if key not in retype:
            k = len(retype)
            retype[key] = (f"%sp_oa{k}", f"%sp_op{k}")
            retype_decl[key] = [f"%sp_oa{k} = OpTypeArray {types[arr][1][0]} {c(want)}",
                                f"%sp_op{k} = OpTypePointer Output %sp_oa{k}"]
            retype_first[key] = v
        out_var_new[v] = retype[key][1]

    # ---- phase code
    o = []
    A_ = o.append
    A_(f"%sp_t0 = OpLoad {uint} {lid_v}")
    A_(f"%sp_vtp0 = OpAccessChain {ptr_wg_uint} %sp_vt %sp_t0")
    A_(f"OpStore %sp_vtp0 {c(0)}")
    A_(f"%sp_pnp0 = OpAccessChain {ptr_wg_uint} %sp_pn %sp_t0")
    A_(f"OpStore %sp_pnp0 {c(0xFFFFFFFF)}")
    o += tri_lines
    o += a_lines
    A_(f"%sp_lt0 = OpULessThan {boolt} %sp_t0 {P}")
    if a_val:
        A_(f"%sp_pv = OpLogicalAnd {boolt} {a_val} %sp_lt0")
    else:
        A_(f"%sp_pv = OpCopyObject {boolt} %sp_lt0")
    A_(f"%sp_te0 = OpCompositeExtract {uint} {tri_val} 0")
    A_(f"%sp_te1 = OpCompositeExtract {uint} {tri_val} 1")
    A_(f"%sp_te2 = OpCompositeExtract {uint} {tri_val} 2")
    A_(f"%sp_tm0 = OpBitwiseAnd {uint} %sp_te0 {c(127)}")
    A_(f"%sp_tm1 = OpBitwiseAnd {uint} %sp_te1 {c(127)}")
    A_(f"%sp_tm2 = OpBitwiseAnd {uint} %sp_te2 {c(127)}")
    A_(f"%sp_ts1 = OpShiftLeftLogical {uint} %sp_tm1 {c(8)}")
    A_(f"%sp_ts2 = OpShiftLeftLogical {uint} %sp_tm2 {c(16)}")
    A_(f"%sp_tpv = OpSelect {uint} %sp_pv %sp_c1 %sp_c0")
    A_(f"%sp_tsv = OpShiftLeftLogical {uint} %sp_tpv {c(24)}")
    A_(f"%sp_tk0 = OpBitwiseOr {uint} %sp_tm0 %sp_ts1")
    A_(f"%sp_tk1 = OpBitwiseOr {uint} %sp_tk0 %sp_ts2")
    A_(f"%sp_tk2 = OpBitwiseOr {uint} %sp_tk1 %sp_tsv")
    A_(f"%sp_tkp = OpAccessChain {ptr_wg_uint} %sp_pk %sp_t0")
    A_("OpStore %sp_tkp %sp_tk2")
    A_(f"OpControlBarrier {c(2)} {c(2)} {c(264)}")
    A_(f"%sp_wg3 = OpLoad {v3uint} {wgid_v}")
    A_(f"%sp_wgx = OpCompositeExtract {uint} %sp_wg3 0")
    A_(f"%sp_ch1 = OpIAdd {uint} %sp_wgx %sp_c1")
    A_(f"%sp_is0 = OpIEqual {boolt} %sp_t0 %sp_c0")
    A_("OpSelectionMerge %sp_pm None")
    A_("OpBranchConditional %sp_is0 %sp_pb %sp_pm")
    A_("%sp_pb = OpLabel")
    A_("OpBranch %sp_lh")
    A_("%sp_lh = OpLabel")
    for nm, init, nxt in (("p", c(0), "%sp_p_n"), ("cur", c(1), "%sp_cur_n"), ("nv", c(0), "%sp_nv_n"),
                          ("np", c(0), "%sp_np_n"), ("mv", c(0), "%sp_mv_n"), ("mp", c(0), "%sp_mp_n")):
        A_(f"%sp_{nm} = OpPhi {uint} {init} %sp_pb {nxt} %sp_lc")
    A_("OpLoopMerge %sp_lm %sp_lc None")
    A_("OpBranch %sp_lcond")
    A_("%sp_lcond = OpLabel")
    A_(f"%sp_go = OpULessThan {boolt} %sp_p {P}")
    A_("OpBranchConditional %sp_go %sp_body %sp_lm")
    A_("%sp_body = OpLabel")
    A_(f"%sp_kp = OpAccessChain {ptr_wg_uint} %sp_pk %sp_p")
    A_(f"%sp_kw = OpLoad {uint} %sp_kp")
    A_(f"%sp_kv = OpShiftRightLogical {uint} %sp_kw {c(24)}")
    A_(f"%sp_isv = OpINotEqual {boolt} %sp_kv %sp_c0")
    A_(f"%sp_i0 = OpBitwiseAnd {uint} %sp_kw {c(127)}")
    A_(f"%sp_k1 = OpShiftRightLogical {uint} %sp_kw {c(8)}")
    A_(f"%sp_i1 = OpBitwiseAnd {uint} %sp_k1 {c(127)}")
    A_(f"%sp_k2 = OpShiftRightLogical {uint} %sp_kw {c(16)}")
    A_(f"%sp_i2 = OpBitwiseAnd {uint} %sp_k2 {c(127)}")
    for k in range(3):
        A_(f"%sp_ap{k} = OpAccessChain {ptr_wg_uint} %sp_vt %sp_i{k}")
        A_(f"%sp_a{k} = OpLoad {uint} %sp_ap{k}")
        A_(f"%sp_g{k} = OpBitwiseAnd {uint} %sp_a{k} {c(255)}")
        A_(f"%sp_nn{k} = OpINotEqual {boolt} %sp_g{k} %sp_cur")
        A_(f"%sp_ex{k}s = OpShiftRightLogical {uint} %sp_a{k} {c(8)}")
        A_(f"%sp_ex{k} = OpBitwiseAnd {uint} %sp_ex{k}s {c(127)}")
    A_(f"%sp_d10 = OpINotEqual {boolt} %sp_i1 %sp_i0")
    A_(f"%sp_d20 = OpINotEqual {boolt} %sp_i2 %sp_i0")
    A_(f"%sp_d21 = OpINotEqual {boolt} %sp_i2 %sp_i1")
    A_(f"%sp_d2 = OpLogicalAnd {boolt} %sp_d20 %sp_d21")
    A_(f"%sp_ne0 = OpCopyObject {boolt} %sp_nn0")
    A_(f"%sp_ne1 = OpLogicalAnd {boolt} %sp_nn1 %sp_d10")
    A_(f"%sp_ne2 = OpLogicalAnd {boolt} %sp_nn2 %sp_d2")
    for k in range(3):
        A_(f"%sp_u{k} = OpSelect {uint} %sp_ne{k} %sp_c1 %sp_c0")
    A_(f"%sp_ua = OpIAdd {uint} %sp_u0 %sp_u1")
    A_(f"%sp_add = OpIAdd {uint} %sp_ua %sp_u2")
    A_(f"%sp_nva = OpIAdd {uint} %sp_nv %sp_add")
    A_(f"%sp_ov1 = OpUGreaterThan {boolt} %sp_nva {c(vmax)}")
    A_(f"%sp_ov2 = OpUGreaterThanEqual {boolt} %sp_np {c(pmax)}")
    A_(f"%sp_ov3 = OpLogicalOr {boolt} %sp_ov1 %sp_ov2")
    A_(f"%sp_ov = OpLogicalAnd {boolt} %sp_ov3 %sp_isv")
    A_(f"%sp_cur1 = OpIAdd {uint} %sp_cur %sp_c1")
    A_(f"%sp_cur2 = OpSelect {uint} %sp_ov %sp_cur1 %sp_cur")
    A_(f"%sp_en0 = OpSelect {boolt} %sp_ov %sp_true %sp_ne0")
    A_(f"%sp_en1 = OpSelect {boolt} %sp_ov %sp_d10 %sp_ne1")
    A_(f"%sp_en2 = OpSelect {boolt} %sp_ov %sp_d2 %sp_ne2")
    for k in range(3):
        A_(f"%sp_eu{k} = OpSelect {uint} %sp_en{k} %sp_c1 %sp_c0")
    A_(f"%sp_eua = OpIAdd {uint} %sp_eu0 %sp_eu1")
    A_(f"%sp_eut = OpIAdd {uint} %sp_eua %sp_eu2")
    A_(f"%sp_nvb = OpSelect {uint} %sp_ov %sp_c0 %sp_nv")
    A_(f"%sp_npb = OpSelect {uint} %sp_ov %sp_c0 %sp_np")
    A_(f"%sp_nv2 = OpIAdd {uint} %sp_nvb %sp_eut")
    A_(f"%sp_np2 = OpIAdd {uint} %sp_npb %sp_c1")
    A_(f"%sp_mine = OpIEqual {boolt} %sp_cur2 %sp_ch1")
    A_(f"%sp_doit = OpLogicalAnd {boolt} %sp_isv %sp_mine")
    A_(f"%sp_m1 = OpIAdd {uint} %sp_mv %sp_eu0")
    A_(f"%sp_m2 = OpIAdd {uint} %sp_m1 %sp_eu1")
    A_("OpSelectionMerge %sp_vm1 None")
    A_("OpBranchConditional %sp_isv %sp_vb1 %sp_vm1")
    A_("%sp_vb1 = OpLabel")
    A_(f"%sp_s0 = OpSelect {uint} %sp_en0 %sp_mv %sp_ex0")
    A_(f"%sp_f1b = OpSelect {uint} %sp_d10 %sp_ex1 %sp_s0")
    A_(f"%sp_s1 = OpSelect {uint} %sp_en1 %sp_m1 %sp_f1b")
    A_(f"%sp_f2a = OpSelect {uint} %sp_d21 %sp_ex2 %sp_s1")
    A_(f"%sp_f2 = OpSelect {uint} %sp_d20 %sp_f2a %sp_s0")
    A_(f"%sp_s2 = OpSelect {uint} %sp_en2 %sp_m2 %sp_f2")
    for k in range(3):
        A_(f"%sp_kp{k} = OpBitwiseAnd {uint} %sp_a{k} {c(0xFF00)}")
        A_(f"%sp_keep{k} = OpBitwiseOr {uint} %sp_kp{k} %sp_cur2")
        A_(f"%sp_sh{k} = OpShiftLeftLogical {uint} %sp_s{k} {c(8)}")
        A_(f"%sp_nw{k}a = OpBitwiseOr {uint} %sp_sh{k} %sp_cur2")
        A_(f"%sp_nw{k} = OpBitwiseOr {uint} %sp_nw{k}a {c(0x8000)}")
        A_(f"%sp_dn{k} = OpLogicalAnd {boolt} %sp_doit %sp_en{k}")
        A_(f"%sp_w{k} = OpSelect {uint} %sp_dn{k} %sp_nw{k} %sp_keep{k}")
    A_(f"OpStore %sp_ap0 %sp_w0")
    A_("OpSelectionMerge %sp_wm1 None")
    A_("OpBranchConditional %sp_d10 %sp_wb1 %sp_wm1")
    A_("%sp_wb1 = OpLabel")
    A_("OpStore %sp_ap1 %sp_w1")
    A_("OpBranch %sp_wm1")
    A_("%sp_wm1 = OpLabel")
    A_("OpSelectionMerge %sp_wm2 None")
    A_("OpBranchConditional %sp_d2 %sp_wb2 %sp_wm2")
    A_("%sp_wb2 = OpLabel")
    A_("OpStore %sp_ap2 %sp_w2")
    A_("OpBranch %sp_wm2")
    A_("%sp_wm2 = OpLabel")
    A_("OpSelectionMerge %sp_wm3 None")
    A_("OpBranchConditional %sp_doit %sp_wb3 %sp_wm3")
    A_("%sp_wb3 = OpLabel")
    A_(f"%sp_q1 = OpShiftLeftLogical {uint} %sp_s1 {c(8)}")
    A_(f"%sp_q2 = OpShiftLeftLogical {uint} %sp_s2 {c(16)}")
    A_(f"%sp_q3 = OpShiftLeftLogical {uint} %sp_mp {c(24)}")
    A_(f"%sp_q4 = OpBitwiseOr {uint} %sp_s0 %sp_q1")
    A_(f"%sp_q5 = OpBitwiseOr {uint} %sp_q4 %sp_q2")
    A_(f"%sp_q6 = OpBitwiseOr {uint} %sp_q5 %sp_q3")
    A_(f"%sp_qp = OpAccessChain {ptr_wg_uint} %sp_pn %sp_p")
    A_("OpStore %sp_qp %sp_q6")
    A_("OpBranch %sp_wm3")
    A_("%sp_wm3 = OpLabel")
    A_("OpBranch %sp_vm1")
    A_("%sp_vm1 = OpLabel")
    # state update (all valid prims advance the chunk state; only mine advance the slot counters)
    A_(f"%sp_cur_n = OpSelect {uint} %sp_isv %sp_cur2 %sp_cur")
    A_(f"%sp_nv_n = OpSelect {uint} %sp_isv %sp_nv2 %sp_nv")
    A_(f"%sp_np_n = OpSelect {uint} %sp_isv %sp_np2 %sp_np")
    A_(f"%sp_mvi = OpSelect {uint} %sp_doit %sp_eut %sp_c0")
    A_(f"%sp_mv_n = OpIAdd {uint} %sp_mv %sp_mvi")
    A_(f"%sp_mpi = OpSelect {uint} %sp_doit %sp_c1 %sp_c0")
    A_(f"%sp_mp_n = OpIAdd {uint} %sp_mp %sp_mpi")
    A_("OpBranch %sp_lc")
    A_("%sp_lc = OpLabel")
    A_(f"%sp_p_n = OpIAdd {uint} %sp_p %sp_c1")
    A_("OpBranch %sp_lh")
    A_("%sp_lm = OpLabel")
    for k, v in ((0, "%sp_mv"), (1, "%sp_mp"), (2, "%sp_cur")):
        A_(f"%sp_cp{k} = OpAccessChain {ptr_wg_uint} %sp_cnt {c(k) if k < 3 else k}")
        A_(f"OpStore %sp_cp{k} {v}")
    A_("OpBranch %sp_pm")
    A_("%sp_pm = OpLabel")
    A_(f"OpControlBarrier {c(2)} {c(2)} {c(264)}")
    for k in range(3):
        A_(f"%sp_rc{k}p = OpAccessChain {ptr_wg_uint} %sp_cnt {c(k)}")
        A_(f"%sp_rc{k} = OpLoad {uint} %sp_rc{k}p")
    A_(f"OpSetMeshOutputsEXT %sp_rc0 %sp_rc1")
    A_(f"%sp_t3 = OpLoad {uint} {lid_v}")
    A_(f"%sp_vtp3 = OpAccessChain {ptr_wg_uint} %sp_vt %sp_t3")
    A_(f"%sp_vtw = OpLoad {uint} %sp_vtp3")
    A_(f"%sp_vss = OpShiftRightLogical {uint} %sp_vtw {c(8)}")
    A_(f"%sp_vs = OpBitwiseAnd {uint} %sp_vss {c(127)}")
    A_(f"%sp_vm = OpBitwiseAnd {uint} %sp_vtw {c(0x8000)}")
    A_(f"%sp_vok = OpINotEqual {boolt} %sp_vm %sp_c0")
    A_(f"%sp_pnp3 = OpAccessChain {ptr_wg_uint} %sp_pn %sp_t3")
    A_(f"%sp_pnw = OpLoad {uint} %sp_pnp3")
    A_(f"%sp_ps = OpShiftRightLogical {uint} %sp_pnw {c(24)}")
    A_(f"%sp_pok = OpINotEqual {boolt} %sp_ps {c(255)}")
    A_(f"%sp_n0 = OpBitwiseAnd {uint} %sp_pnw {c(255)}")
    A_(f"%sp_n1s = OpShiftRightLogical {uint} %sp_pnw {c(8)}")
    A_(f"%sp_n1r = OpBitwiseAnd {uint} %sp_n1s {c(255)}")
    A_(f"%sp_n1 = OpCopyObject {uint} {'%sp_n0' if mutant else '%sp_n1r'}")
    A_(f"%sp_n2s = OpShiftRightLogical {uint} %sp_pnw {c(16)}")
    A_(f"%sp_n2 = OpBitwiseAnd {uint} %sp_n2s {c(255)}")
    A_(f"%sp_ntri = OpCompositeConstruct {v3uint} %sp_n0 %sp_n1 %sp_n2")
    for k in range(3):
        A_(f"%sp_nt{k} = OpCompositeExtract {uint} %sp_ntri {k}")
    cap_slots = {}
    if capture:
        consts_val = {}
        for ln in lines:
            m_ = re.match(r"\s*(%\w+) = OpConstant " + re.escape(uint) + r" (\d+)$", ln)
            if m_:
                consts_val[m_.group(1)] = int(m_.group(2))
        off = 0
        for i in sorted(capture_after):
            for kind, ptr, val, var, (pt, rest) in capture_after[i]:
                if kind != "v" or (var, rest) in cap_slots:
                    continue
                t = types[types[pt][1][1]]
                if t[0] == "OpTypeVector" and types[t[1][0]] == ("OpTypeFloat", ["32"], types[t[1][0]][2]):
                    comps = int(t[1][1])
                elif t[0] == "OpTypeFloat" and t[1] == ["32"]:
                    comps = 1
                else:
                    raise SplitError(f"capture: output {var} {rest} is not float or float vector")
                member = consts_val.get(rest.split()[0]) if rest else None
                cap_slots[(var, rest)] = {"offset": off, "comps": comps, "member": member}
                off += comps
        vs_data = off
    else:
        vs_data = 0
    vs_words = vs_data + (1 if capture else 0)
    rec_words = 8 + vmax * vs_words + pmax * 8
    if capture:
        floatt = next(k for k, (op, a, _) in types.items() if op == "OpTypeFloat" and a == ["32"])
        decl.append(f"%sp_capp = OpTypePointer StorageBuffer {uint}")
        o += [f"%sp_cg = OpAccessChain %sp_ppay_v3 %sp_payload {c(0)}", f"%sp_cn = OpAccessChain %sp_ppay_v3 %sp_payload {c(1)}",
              f"%sp_cgv = OpLoad {v3uint} %sp_cg", f"%sp_cnv = OpLoad {v3uint} %sp_cn",
              f"%sp_gx = OpCompositeExtract {uint} %sp_cgv 0", f"%sp_gy = OpCompositeExtract {uint} %sp_cgv 1",
              f"%sp_gz = OpCompositeExtract {uint} %sp_cgv 2", f"%sp_nx = OpCompositeExtract {uint} %sp_cnv 0",
              f"%sp_ny = OpCompositeExtract {uint} %sp_cnv 1",
              f"%sp_l1 = OpIMul {uint} %sp_gy %sp_nx", f"%sp_nxy = OpIMul {uint} %sp_nx %sp_ny",
              f"%sp_l2 = OpIMul {uint} %sp_gz %sp_nxy", f"%sp_l3 = OpIAdd {uint} %sp_gx %sp_l1",
              f"%sp_lin = OpIAdd {uint} %sp_l3 %sp_l2", f"%sp_lk = OpIMul {uint} %sp_lin {c(chunks)}",
              f"%sp_rec = OpIAdd {uint} %sp_lk %sp_wgx", f"%sp_base = OpIMul {uint} %sp_rec {c(rec_words)}",
              f"%sp_vb0 = OpIMul {uint} %sp_vs {c(vs_words)}", f"%sp_vb1c = OpIAdd {uint} %sp_base {c(8)}",
              f"%sp_vbase = OpIAdd {uint} %sp_vb1c %sp_vb0",
              f"%sp_pb0 = OpIMul {uint} %sp_ps {c(8)}", f"%sp_pb1 = OpIAdd {uint} %sp_base {c(8 + vmax * vs_words)}",
              f"%sp_pbase = OpIAdd {uint} %sp_pb1 %sp_pb0"]
        def wr(word_idx_id, val, tag):
            return [f"%sp_cw{tag} = OpAccessChain %sp_capp %sp_cap {c(0)} {word_idx_id}", f"OpStore %sp_cw{tag} {val}"]
        o += [f"%sp_h0 = OpIEqual {boolt} %sp_t3 %sp_c0", "OpSelectionMerge %sp_hm None",
              "OpBranchConditional %sp_h0 %sp_hb %sp_hm", "%sp_hb = OpLabel"]
        for k, val in enumerate(("%sp_rc0", "%sp_rc1", "%sp_rc2", V, P)):
            o.append(f"%sp_hi{k} = OpIAdd {uint} %sp_base {c(k)}")
            o += wr(f"%sp_hi{k}", val, f"h{k}")
        o += ["OpBranch %sp_hm", "%sp_hm = OpLabel"]
        o += ["OpSelectionMerge %sp_pm2 None", f"OpBranchConditional %sp_pok %sp_pcb %sp_pm2", "%sp_pcb = OpLabel"]
        for k, val in enumerate(("%sp_nt0", "%sp_nt1", "%sp_nt2", "%sp_tm0", "%sp_tm1", "%sp_tm2")):
            o.append(f"%sp_pi{k} = OpIAdd {uint} %sp_pbase {c(k)}")
            o += wr(f"%sp_pi{k}", val, f"p{k}")
        o += ["OpBranch %sp_pm2", "%sp_pm2 = OpLabel"]
        o += ["OpSelectionMerge %sp_vm2 None", "OpBranchConditional %sp_vok %sp_vcb %sp_vm2", "%sp_vcb = OpLabel",
              f"%sp_vwi = OpIAdd {uint} %sp_vbase {c(vs_data)}"]
        o += wr("%sp_vwi", "%sp_t3", "vl")
        o += ["OpBranch %sp_vm2", "%sp_vm2 = OpLabel"]

    # ---- assemble the new module
    out = list(lines)
    for v, npt in out_var_new.items():
        i = one(r"^\s*" + re.escape(v) + r" = OpVariable ", "output variable " + v)
        out[i] = re.sub(r"OpVariable %\w+ Output", "OpVariable " + npt + " Output", out[i])
    for i, ln in new_region.items():
        out[i] = "         " + ln
    # the payload replaces the two builtins in the program, everywhere except my own chunk-id load
    cnt = [0]
    for i in range(fstart, fend):
        mc = re.match(r"\s*(%\w+) = OpAccessChain (%\w+) (%\w+) (%\w+)$", out[i])
        if mc and mc.group(3) in (wgid_v, nwg_v):
            member = c(0) if mc.group(3) == wgid_v else c(1)
            out[i] = f"         {mc.group(1)} = OpAccessChain %sp_ppay_u %sp_payload {member} {mc.group(4)}"
            continue
        mm = re.match(r"\s*(%\w+) = OpLoad " + re.escape(v3uint) + r" (%\w+)$", out[i])
        if mm and mm.group(2) in (wgid_v, nwg_v):
            member = c(0) if mm.group(2) == wgid_v else c(1)
            k = cnt[0]
            cnt[0] += 1
            out[i] = (f"         %sp_pa{k} = OpAccessChain %sp_ppay_v3 %sp_payload {member}\n"
                      f"         {mm.group(1)} = OpLoad {v3uint} %sp_pa{k}")
    if capture:
        floatt = next(k for k, (op, a, _) in types.items() if op == "OpTypeFloat" and a == ["32"])
        for i, items in capture_after.items():
            extra = []
            for kind, ptr, val, var, (pt, rest) in items:
                uniq = f"{i}"
                if kind == "v":
                    slot = cap_slots[(var, rest)]
                    for comp in range(slot["comps"]):
                        t = f"%spx{uniq}_{slot['offset']}_{comp}"
                        src = val if slot["comps"] == 1 else t + "e"
                        pre = [] if slot["comps"] == 1 else [f"{t}e = OpCompositeExtract {floatt} {val} {comp}"]
                        extra += pre + [f"{t}b = OpBitcast {uint} {src}",
                                        f"{t}i = OpIAdd {uint} %sp_vbase {c(slot['offset'] + comp)}",
                                        f"{t}p = OpAccessChain %sp_capp %sp_cap {c(0)} {t}i", f"OpStore {t}p {t}b"]
                else:
                    t = f"%spc{uniq}"
                    extra += [f"{t}s = OpSelect {uint} {val} %sp_c1 %sp_c0", f"{t}i = OpIAdd {uint} %sp_pbase {c(6)}",
                              f"{t}p = OpAccessChain %sp_capp %sp_cap {c(0)} {t}i", f"OpStore {t}p {t}s"]
            out[i] = out[i] + "\n" + "\n".join("         " + x for x in extra)
    out[smo] = "\n".join("         " + x for x in o)
    out[ep] = re.sub(r"(OpEntryPoint MeshEXT %\w+ \"\w+\"(?: %\w+)*)", lambda m_: m_.group(1) + " " + " ".join(new_ifaces), out[ep])
    out[em_v] = re.sub(r"OutputVertices \d+", f"OutputVertices {vmax}", out[em_v])
    out[em_p] = re.sub(r"OutputPrimitivesEXT \d+", f"OutputPrimitivesEXT {pmax}", out[em_p])
    ins = []
    uint_line = types[uint][2]
    ins.append((uint_line + 1, [f"{consts[v]} = OpConstant {uint} {v}" for v in sorted(consts)]))
    for key, lines_ in retype_decl.items():
        vi = next(i for i in range(n) if re.match(r"\s*" + re.escape(retype_first[key]) + r" = OpVariable ", lines[i]))
        ins.append((vi, lines_))
    first_fn_i = find(lines, r"^\s*%\w+ = OpFunction")
    ins.append((first_fn_i, decl))
    for at, items in sorted(ins, key=lambda x: -x[0]):
        out[at:at] = ["         " + d for d in items]
    if add_nwg:
        out = insert_before(out, r"OpDecorate %\w+ BuiltIn", ["OpDecorate %sp_nwv BuiltIn NumWorkgroups"])
    if capture:
        out = insert_before(out, r"OpDecorate %\w+ BuiltIn", [
            f"OpDecorate %sp_cap_rt ArrayStride 4", "OpMemberDecorate %sp_cap_st 0 Offset 0", "OpDecorate %sp_cap_st Block",
            f"OpDecorate %sp_cap DescriptorSet {CAPTURE_SET}", "OpDecorate %sp_cap Binding 0"])
    text2 = "\n".join(out)
    mesh = assemble(text2)
    task = assemble(task_module(chunks))
    vmap = []
    for (var, rest), sl in cap_slots.items():
        loc = next((int(m_.group(1)) for m_ in (re.match(r"\s*OpDecorate " + re.escape(var) + r" Location (\d+)", x) for x in lines) if m_), None)
        bi = next((m_.group(1) for m_ in (re.match(r"\s*OpDecorate " + re.escape(var) + r" BuiltIn (\w+)", x) for x in lines) if m_), None)
        vmap.append({"offset": sl["offset"], "comps": sl["comps"], "member": sl["member"], "location": loc, "builtin": bi})
    rep = {"chunks": chunks, "vmax": vmax, "pmax": pmax, "vertex_outputs": len(vert_vars), "prim_outputs": len(prim_vars),
           "tri_cone_lines": len(tri_lines), "capture": capture, "vmap": vmap, "vs_words": vs_words, "rec_words": rec_words}
    return mesh, task, rep


def insert_before(lines, pat, new, keep=False):
    rx = re.compile(pat)
    for i, ln in enumerate(lines):
        if rx.search(ln):
            return lines[:i] + new + lines[i:]
    raise SplitError("no anchor " + pat)


def capture_header(c, uint):
    return []


def mirror_stores(out, fstart, fend, smo, vert_ac, prim_ac, idx_v, out_vars, types, c, uint, cap_vars, idx_store,
                  per_prim_struct):
    raise SplitError("capture mode is not implemented yet")


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("spv")
    ap.add_argument("--out", required=True)
    ap.add_argument("--chunks", type=int, default=3)
    ap.add_argument("--vmax", type=int, default=64)
    ap.add_argument("--pmax", type=int, default=64)
    ap.add_argument("--capture", action="store_true")
    a = ap.parse_args()
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import spv
    w = spv.load(a.spv)
    mesh, task, rep = split_mesh(w, a.chunks, a.vmax, a.pmax, a.capture)
    spv.save(mesh, a.out + ".mesh.spv")
    spv.save(task, a.out + ".task.spv")
    print(rep)
    for nm, x in (("mesh", mesh), ("task", task)):
        ok, msg = validate(x)
        print(nm, "spirv-val vulkan1.3:", "ok" if ok else msg)


if __name__ == "__main__":
    main()
