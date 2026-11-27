"""--vs-lane-from-ballot: vertex shaders without the SubgroupLocalInvocationId built-in (port/shaders_vk).

A Metal vertex function cannot declare thread_index_in_simdgroup, so MoltenVK refuses the built-in in a vertex
stage ("Subgroup builtins are not available in this type of function"). The lane is computed once at the top of
the entry block instead, where every launched invocation of the subgroup is active: the invocation's rank among the
active lanes (OpGroupNonUniformIAdd ExclusiveScan of 1, Metal simd_prefix_exclusive_sum) picks that rank's set bit
of the entry ballot, and that bit's index is the lane. Exact for subgroups of up to 64 lanes (Apple: 32).
"""
import re

_STEPS = (16, 8, 4, 2, 1)


def _find(lines, pattern):
    rx = re.compile(pattern)
    for line in lines:
        m = rx.match(line)
        if m:
            return m.group(1)
    return None


def rewrite(text):
    if "BuiltIn SubgroupLocalInvocationId" not in text:
        return text
    lines = text.split("\n")
    var_at = next((i for i, l in enumerate(lines)
                   if re.match(r"\s*%sg_lane = OpVariable %_ptr_Input_uint Input\s*$", l)), None)
    entry_at = next((i for i, l in enumerate(lines) if re.match(r"\s*%entry = OpLabel\s*$", l)), None)
    if var_at is None or entry_at is None:
        raise ValueError("vs-lane-from-ballot: no %sg_lane input or no %entry block")
    if not _find(lines, r"\s*(%bool) = OpTypeBool\s*$") or not _find(lines, r"\s*(%true) = OpConstantTrue %bool\s*$"):
        raise ValueError("vs-lane-from-ballot: module has no %bool/%true")

    decls = []
    v4 = _find(lines, r"\s*(%\S+) = OpTypeVector %uint 4\s*$")
    if v4 is None:
        v4 = "%vl_v4uint"
        decls.append(f"{v4} = OpTypeVector %uint 4")
    priv = "%vl_ptr_priv_uint"
    decls.append(f"{priv} = OpTypePointer Private %uint")
    consts = {0: "%vl_k0", 1: "%vl_k1", 3: "%vl_k3", 32: "%vl_k32"}
    for s in _STEPS:
        consts.setdefault(s, f"%vl_k{s}")
        consts.setdefault((1 << s) - 1, f"%vl_m{s}")
    decls += [f"{name} = OpConstant %uint {value}" for value, name in sorted(consts.items())]
    k = consts

    init = [f"%vl_bal = OpGroupNonUniformBallot {v4} {k[3]} %true",
            f"%vl_rank = OpGroupNonUniformIAdd %uint {k[3]} ExclusiveScan {k[1]}",
            f"%vl_w0 = OpCompositeExtract %uint %vl_bal 0",
            f"%vl_w1 = OpCompositeExtract %uint %vl_bal 1",
            f"%vl_n0 = OpBitCount %uint %vl_w0",
            f"%vl_hi = OpUGreaterThanEqual %bool %vl_rank %vl_n0",
            f"%vl_rhi = OpISub %uint %vl_rank %vl_n0",
            f"%vl_wa = OpSelect %uint %vl_hi %vl_w1 %vl_w0",
            f"%vl_ra = OpSelect %uint %vl_hi %vl_rhi %vl_rank",
            f"%vl_pa = OpSelect %uint %vl_hi {k[32]} {k[0]}"]
    w, r, p = "%vl_wa", "%vl_ra", "%vl_pa"
    for s in _STEPS:
        init += [f"%vl_lo{s} = OpBitwiseAnd %uint {w} {k[(1 << s) - 1]}",
                 f"%vl_c{s} = OpBitCount %uint %vl_lo{s}",
                 f"%vl_up{s} = OpUGreaterThanEqual %bool {r} %vl_c{s}",
                 f"%vl_rs{s} = OpISub %uint {r} %vl_c{s}",
                 f"%vl_ws{s} = OpShiftRightLogical %uint {w} {k[s]}",
                 f"%vl_ps{s} = OpIAdd %uint {p} {k[s]}",
                 f"%vl_r{s} = OpSelect %uint %vl_up{s} %vl_rs{s} {r}",
                 f"%vl_w{s}x = OpSelect %uint %vl_up{s} %vl_ws{s} {w}",
                 f"%vl_p{s} = OpSelect %uint %vl_up{s} %vl_ps{s} {p}"]
        w, r, p = f"%vl_w{s}x", f"%vl_r{s}", f"%vl_p{s}"
    init.append(f"OpStore %sg_lane {p}")

    body_at = entry_at + 1
    while body_at < len(lines) and re.match(r"\s*%\S+ = OpVariable \S+ Function", lines[body_at]):
        body_at += 1
    out = lines[:body_at] + init + lines[body_at:]
    out[var_at] = f"%sg_lane = OpVariable {priv} Private"
    out[var_at:var_at] = decls
    out = [l for l in out if not re.match(r"\s*OpDecorate %sg_lane (BuiltIn SubgroupLocalInvocationId|Flat)\s*$", l)]

    have = set(re.findall(r"OpCapability (\w+)", text))
    need = [c for c in ("GroupNonUniform", "GroupNonUniformBallot", "GroupNonUniformArithmetic") if c not in have]
    last_cap = max(i for i, l in enumerate(out) if re.match(r"\s*OpCapability ", l))
    out[last_cap + 1:last_cap + 1] = [f"OpCapability {c}" for c in need]
    return "\n".join(out)
