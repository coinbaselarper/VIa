#!/usr/bin/env python3
"""A pixel shader in the translator's shape (rdna2spirv.py `export`) for the output-variant render test.

Registers r0..r7 are elements of a Function uint array: r[k] = pc.v[k >> 2][k & 3] + uint(gl_FragCoord.x) * pc.scale
(scale 0 in the test: per-draw values that the compiler cannot fold). Location 0 is a compressed export
`exp mrt0 pkrtz(r0, r1), pkrtz(r2, r3) compr` with the translator's software f32->f16 (round toward zero), stored
through a register with an exec select, decoded with UnpackHalf2x16; location 1 a plain export
`exp mrt1 r4, r5, r6, r7` (the dwords bit-cast to float). --branch: the packs in a conditional block before the
export's block (the translator's "any lane active" shape). Prints SPIR-V assembly."""
import sys

L = []


def e(s):
    L.append(s)


n = [0]


def t():
    n[0] += 1
    return "%%t%d" % n[0]


def f16(a):
    """The translator's v_cvt_pkrtz half: a is a uint id holding the f32 bits."""
    sh = t(); e(f"{sh} = OpShiftRightLogical %uint {a} %u16")
    sign = t(); e(f"{sign} = OpBitwiseAnd %uint {sh} %u32768")
    ab = t(); e(f"{ab} = OpBitwiseAnd %uint {a} %u7fffffff")
    s1 = t(); e(f"{s1} = OpISub %uint {ab} %u939524096")
    nor = t(); e(f"{nor} = OpShiftRightLogical %uint {s1} %u13")
    ex = t(); e(f"{ex} = OpShiftRightLogical %uint {ab} %u23")
    ma = t(); e(f"{ma} = OpBitwiseAnd %uint {ab} %u8388607")
    mi = t(); e(f"{mi} = OpBitwiseOr %uint %u8388608 {ma}")
    sb = t(); e(f"{sb} = OpISub %uint %u126 {ex}")
    lt = t(); e(f"{lt} = OpULessThan %bool {sb} %u25")
    sc = t(); e(f"{sc} = OpSelect %uint {lt} {sb} %u25")
    den = t(); e(f"{den} = OpShiftRightLogical %uint {mi} {sc}")
    c1 = t(); e(f"{c1} = OpUGreaterThanEqual %bool {ab} %u947912704")
    v1 = t(); e(f"{v1} = OpSelect %uint {c1} {nor} {den}")
    c2 = t(); e(f"{c2} = OpUGreaterThanEqual %bool {ab} %u1199570944")
    v2 = t(); e(f"{v2} = OpSelect %uint {c2} %u31743 {v1}")
    c3 = t(); e(f"{c3} = OpIEqual %bool {ab} %u2139095040")
    v3 = t(); e(f"{v3} = OpSelect %uint {c3} %u31744 {v2}")
    q1 = t(); e(f"{q1} = OpShiftRightLogical %uint {ab} %u13")
    q2 = t(); e(f"{q2} = OpBitwiseAnd %uint {q1} %u1023")
    q3 = t(); e(f"{q3} = OpBitwiseOr %uint {q2} %u32256")
    c4 = t(); e(f"{c4} = OpUGreaterThan %bool {ab} %u2139095040")
    v4 = t(); e(f"{v4} = OpSelect %uint {c4} {q3} {v3}")
    r = t(); e(f"{r} = OpBitwiseOr %uint {v4} {sign}")
    return r


def reg_ptr(k):
    p = t()
    e(f"{p} = OpAccessChain %ptr_f_uint %regs %u{k}")
    return p


def load_reg(k):
    v = t()
    e(f"{v} = OpLoad %uint {reg_ptr(k)}")
    return v


def store_reg(k, v):
    e(f"OpStore {reg_ptr(k)} {v}")


def pk(k0, k1, dst):
    a = load_reg(k0)
    fa = t(); e(f"{fa} = OpBitcast %float {a}")
    b = load_reg(k1)
    fb = t(); e(f"{fb} = OpBitcast %float {b}")
    ua = t(); e(f"{ua} = OpBitcast %uint {fa}")
    lo = f16(ua)
    ub = t(); e(f"{ub} = OpBitcast %uint {fb}")
    hi = f16(ub)
    s = t(); e(f"{s} = OpShiftLeftLogical %uint {hi} %u16")
    d = t(); e(f"{d} = OpBitwiseOr %uint {lo} {s}")
    old = load_reg(dst)
    sel = t(); e(f"{sel} = OpSelect %uint %true {d} {old}")
    store_reg(dst, sel)


def main():
    consts = {16: 16, 32768: 32768, "7fffffff": 0x7FFFFFFF, 939524096: 939524096, 13: 13, 23: 23, 8388607: 8388607,
              8388608: 8388608, 126: 126, 25: 25, 947912704: 947912704, 1199570944: 1199570944, 31743: 31743,
              2139095040: 2139095040, 31744: 31744, 1023: 1023, 32256: 32256}
    for k in range(11):
        consts.setdefault(k, k)
    head = """OpCapability Shader
%glsl = OpExtInstImport "GLSL.std.450"
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %out0 %out1 %fc %pc
OpExecutionMode %main OriginUpperLeft
OpDecorate %out0 Location 0
OpDecorate %out1 Location 1
OpDecorate %fc BuiltIn FragCoord
OpDecorate %PC Block
OpMemberDecorate %PC 0 Offset 0
OpMemberDecorate %PC 1 Offset 32
OpDecorate %arr_v4uint_2 ArrayStride 16
%void = OpTypeVoid
%fn = OpTypeFunction %void
%bool = OpTypeBool
%true = OpConstantTrue %bool
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%v2float = OpTypeVector %float 2
%v4float = OpTypeVector %float 4
%v4uint = OpTypeVector %uint 4
"""
    body_consts = "".join("%%u%s = OpConstant %%uint %d\n" % (k, v) for k, v in consts.items())
    head2 = """%arr_v4uint_2 = OpTypeArray %v4uint %u2
%PC = OpTypeStruct %arr_v4uint_2 %uint
%ptr_pc = OpTypePointer PushConstant %PC
%ptr_pc_uint = OpTypePointer PushConstant %uint
%arr_uint_10 = OpTypeArray %uint %u10
%ptr_f_arr = OpTypePointer Function %arr_uint_10
%ptr_f_uint = OpTypePointer Function %uint
%ptr_out_v4 = OpTypePointer Output %v4float
%ptr_in_v4 = OpTypePointer Input %v4float
%f0 = OpConstant %float 0
%null4 = OpConstantNull %v4float
%pc = OpVariable %ptr_pc PushConstant
%out0 = OpVariable %ptr_out_v4 Output %null4
%out1 = OpVariable %ptr_out_v4 Output %null4
%fc = OpVariable %ptr_in_v4 Input
%main = OpFunction %void None %fn
%entry = OpLabel
%regs = OpVariable %ptr_f_arr Function
"""
    e("%fcv = OpLoad %v4float %fc")
    e("%fx = OpCompositeExtract %float %fcv 0")
    e("%ux = OpConvertFToU %uint %fx")
    e("%sp = OpAccessChain %ptr_pc_uint %pc %u1")
    e("%scale = OpLoad %uint %sp")
    e("%mx = OpIMul %uint %ux %scale")
    for k in range(8):
        p = t()
        e(f"{p} = OpAccessChain %ptr_pc_uint %pc %u0 %u{k >> 2} %u{k & 3}")
        v = t(); e(f"{v} = OpLoad %uint {p}")
        s = t(); e(f"{s} = OpIAdd %uint {v} %mx")
        store_reg(k, s)
    store_reg(8, "%u0")
    store_reg(9, "%u0")
    branch = "--branch" in sys.argv
    if branch:
        e("%any = OpIEqual %bool %scale %u0")
        e("OpSelectionMerge %merge None")
        e("OpBranchConditional %any %packs %merge")
        e("%packs = OpLabel")
    pk(0, 1, 8)
    pk(2, 3, 9)
    if branch:
        e("OpBranch %merge")
        e("%merge = OpLabel")
    comps = []
    for dst in (8, 9):
        d = load_reg(dst)
        u = t(); e(f"{u} = OpExtInst %v2float %glsl UnpackHalf2x16 {d}")
        for c in (0, 1):
            x = t(); e(f"{x} = OpCompositeExtract %float {u} {c}")
            comps.append(x)
    old = t(); e(f"{old} = OpLoad %v4float %out0")
    v = t(); e(f"{v} = OpCompositeConstruct %v4float {' '.join(comps)}")
    sel = t(); e(f"{sel} = OpSelect %v4float %true {v} {old}")
    e(f"OpStore %out0 {sel}")
    comps = []
    for k in range(4, 8):
        r = load_reg(k)
        x = t(); e(f"{x} = OpBitcast %float {r}")
        comps.append(x)
    old = t(); e(f"{old} = OpLoad %v4float %out1")
    v = t(); e(f"{v} = OpCompositeConstruct %v4float {' '.join(comps)}")
    sel = t(); e(f"{sel} = OpSelect %v4float %true {v} {old}")
    e(f"OpStore %out1 {sel}")
    e("OpReturn")
    e("OpFunctionEnd")
    sys.stdout.write(head + body_consts + head2 + "\n".join(L) + "\n")


if __name__ == "__main__":
    main()
