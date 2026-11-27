#!/usr/bin/env python3
"""Compute shaders whose wave64-emulation exchange buffer (%x_buf, 2L + 6W + 7 words for L threads and W = L / 64
waves) does not fit next to a full 32 KiB LDS: the buffer moves from threadgroup memory to a storage buffer
(descriptor set 0, binding 29), one slice of x_buf_words per workgroup indexed by the linear workgroup id.

SPIR-V transform of the corpus module (no translator change): the Workgroup array variable is dropped, every
OpAccessChain on it becomes an access chain into the storage buffer at (slice base + index), the base is computed once
at function entry. The emulated scheduler's barriers already carry UniformMemory semantics (0x948).
Runtime contract: bind a buffer of x_buf_words * 4 * (groups x * y * z) bytes at binding 29 (set 30 after
split_bindings) for every dispatch; the contents need not survive.
"""
import re, sys

sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
import mesh_split as M  # noqa: E402

BINDING = 29


def x_buf_to_device(words):
    text = M.disassemble(words)
    lines = text.split("\n")
    n = len(lines)
    m = re.search(r"OpExecutionMode %\w+ LocalSize (\d+) (\d+) (\d+)", text)
    if not m:
        raise M.SplitError("no LocalSize")
    L = int(m.group(1)) * int(m.group(2)) * int(m.group(3))
    W = -(-L // 64)
    want = 2 * L + 6 * W + 7
    consts = {}
    types = {}
    for ln in lines:
        mm = re.match(r"\s*(%\w+) = OpConstant (%\w+) (\d+)$", ln)
        if mm:
            consts[mm.group(1)] = int(mm.group(3))
        mm = re.match(r"\s*(%\w+) = OpTypeArray (%\w+) (%\w+)$", ln)
        if mm:
            types[mm.group(1)] = (mm.group(2), consts.get(mm.group(3)))
    ptrs = {}
    for ln in lines:
        mm = re.match(r"\s*(%\w+) = OpTypePointer Workgroup (%\w+)$", ln)
        if mm:
            ptrs[mm.group(1)] = mm.group(2)
    xvar = None
    for i, ln in enumerate(lines):
        mm = re.match(r"\s*(%\w+) = OpVariable (%\w+) Workgroup$", ln)
        if mm and ptrs.get(mm.group(2)) in types and types[ptrs[mm.group(2)]][1] == want:
            if xvar:
                raise M.SplitError("more than one workgroup array of the exchange-buffer size")
            xvar = (mm.group(1), i, types[ptrs[mm.group(2)]][0])
    if not xvar:
        raise M.SplitError(f"no Workgroup array of {want} words (L = {L})")
    v, vline, uint = xvar
    ac = re.compile(r"^(\s*)(%\w+) = OpAccessChain (%\w+) " + re.escape(v) + r" (%\w+)$")
    sites = [i for i, ln in enumerate(lines) if ac.match(ln)]
    other = [i for i, ln in enumerate(lines) if re.search(re.escape(v) + r"(?!\w)", ln) and i != vline and i not in sites
             and not ln.lstrip().startswith("OpEntryPoint") and "OpName" not in ln]
    if other:
        raise M.SplitError("exchange buffer used other than by access chain: " + lines[other[0]].strip()[:100])
    firstfn = next(i for i, ln in enumerate(lines) if re.match(r"\s*%\w+ = OpFunction", ln))
    ep = next(i for i, ln in enumerate(lines) if "OpEntryPoint GLCompute" in ln)
    fn = re.match(r"\s*OpEntryPoint GLCompute (%\w+)", lines[ep]).group(1)
    mainstart = next(i for i, ln in enumerate(lines) if re.match(r"\s*" + re.escape(fn) + r" = OpFunction", ln))
    builtin = {}
    for ln in lines:
        mm = re.match(r"\s*OpDecorate (%\w+) BuiltIn (\w+)", ln)
        if mm:
            builtin[mm.group(2)] = mm.group(1)
    wg = builtin["WorkgroupId"]
    extra_decl, extra_dec = [], []
    if "NumWorkgroups" in builtin:
        nwg = builtin["NumWorkgroups"]
    else:
        nwg = "%xb_nwv"
        wgtype = next(re.match(r"\s*" + re.escape(wg) + r" = OpVariable (%\w+) Input", ln).group(1) for ln in lines
                      if re.match(r"\s*" + re.escape(wg) + r" = OpVariable (%\w+) Input", ln))
        extra_decl = [f"%xb_nwv = OpVariable {wgtype} Input"]
        extra_dec = ["OpDecorate %xb_nwv BuiltIn NumWorkgroups"]
    v3 = next(re.match(r"\s*(%\w+)", ln).group(1) for ln in lines if re.match(r"\s*%\w+ = OpTypeVector " + re.escape(uint) + " 3$", ln))
    pos = mainstart + 1
    while not re.match(r"\s*%\w+ = OpLabel", lines[pos]):
        pos += 1
    pos += 1
    while re.match(r"\s*%\w+ = OpVariable ", lines[pos]):
        pos += 1
    entry = [f"%xb_wg = OpLoad {v3} {wg}", f"%xb_nw = OpLoad {v3} {nwg}",
             f"%xb_x = OpCompositeExtract {uint} %xb_wg 0", f"%xb_y = OpCompositeExtract {uint} %xb_wg 1",
             f"%xb_z = OpCompositeExtract {uint} %xb_wg 2", f"%xb_nx = OpCompositeExtract {uint} %xb_nw 0",
             f"%xb_ny = OpCompositeExtract {uint} %xb_nw 1", f"%xb_zy = OpIMul {uint} %xb_z %xb_ny",
             f"%xb_zy2 = OpIAdd {uint} %xb_zy %xb_y", f"%xb_l = OpIMul {uint} %xb_zy2 %xb_nx",
             f"%xb_lin = OpIAdd {uint} %xb_l %xb_x", f"%xb_base = OpIMul {uint} %xb_lin %xb_len",
             "OpStore %xb_basev %xb_base"]
    decl = extra_decl + [f"%xb_len = OpConstant {uint} {want}", f"%xb_c0 = OpConstant {uint} 0",
            f"%xb_rt = OpTypeRuntimeArray {uint}", "%xb_st = OpTypeStruct %xb_rt", "%xb_pst = OpTypePointer StorageBuffer %xb_st",
            f"%xb_pu = OpTypePointer StorageBuffer {uint}", "%xb_dev = OpVariable %xb_pst StorageBuffer",
            f"%xb_ppriv = OpTypePointer Private {uint}", f"%xb_basev = OpVariable %xb_ppriv Private %xb_c0"]
    out = list(lines)
    for k, i in enumerate(sites):
        mm = ac.match(lines[i])
        ind, r, _, idx = mm.groups()
        out[i] = f"{ind}%xb_b{k} = OpLoad {uint} %xb_basev\n{ind}%xb_i{k} = OpIAdd {uint} %xb_b{k} {idx}\n{ind}{r} = OpAccessChain %xb_pu %xb_dev %xb_c0 %xb_i{k}"
    out[vline] = ""
    out[pos:pos] = ["         " + e for e in entry]
    out[ep] = re.sub(r"(\s)" + re.escape(v) + r"(?=\s|$)", "", out[ep]) + " %xb_dev %xb_basev" + (" %xb_nwv" if extra_decl else "")
    anchor = next(i for i, ln in enumerate(lines) if re.match(r"\s*%\w+ = OpVariable ", ln) and "Function" not in ln)
    out[anchor:anchor] = ["         " + d for d in decl]
    dec = next(i for i, ln in enumerate(lines) if re.match(r"\s*OpDecorate %\w+ BuiltIn", ln))
    out[dec:dec] = extra_dec + ["OpDecorate %xb_rt ArrayStride 4", "OpMemberDecorate %xb_st 0 Offset 0", "OpDecorate %xb_st Block",
                    f"OpDecorate %xb_dev DescriptorSet 0", f"OpDecorate %xb_dev Binding {BINDING}"]
    res = M.assemble("\n".join(out))
    return res, {"x_buf_words": want, "sites": len(sites), "binding": BINDING}
