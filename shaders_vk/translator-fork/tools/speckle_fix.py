"""translator-fork: correctness lowerings for the Metal path (speckle / correctness worker, round 15).

--image-atomic64   two-VGPR (dmask:0x3) image_atomic_umax/umin on a 2D image: a 64-bit atomic on the texel, the
                   high dword deciding. SPIR-V for Vulkan 1.x/SPIRV-Cross has no 64-bit image atomic, so the op is
                   emitted as a marked OpImageWrite through the 2D uint storage view of the same bindless index
                   (binding 9, %stv1_0); shaderc_msl/tools/speckle_msl.py `atomic64_texture` turns every marked write
                   into texture2d<ulong, access::read_write>::atomic_max/atomic_min on that RG32Uint texture and
                   refuses a library with a marker left. Only lanes that are on, a found descriptor and a T# format of
                   32_32_UINT touch memory (as the console: a 64-bit image atomic needs that format).
(early depth from DB_SHADER_CONTROL is added at MSL level: shaderc_msl/tools/speckle_msl.py --early-depth)
--cs-implicit-lod  compute: image_sample / _b / _c / _c_b (implicit LOD) take their LOD from quad derivatives of the
                   coordinates (lanes ^1, ^2 of the console quad) plus the bias, as the console does, instead of
                   LOD 0.
--mesh-index-clamp mesh epilogue: each primitive's three 9-bit vertex indices are clamped below the vertex count,
                   and a mesh with no vertices emits no primitives.
"""

ON = {"atomic64": False, "cs_implicit_lod": False, "mesh_index_clamp": False}

ATOMIC64_MARK = {"image_atomic_umax": 0x5CA1AB1E, "image_atomic_umin": 0x5CA1AB1D}
ATOMIC64_MARK2 = 0x64A7015C
GFX10_FORMAT_32_32_UINT = 62


def add_arguments(ap):
    ap.add_argument("--image-atomic64", action="store_true",
                    help="translator-fork: two-VGPR image_atomic_umax/umin on 2D images are 64-bit atomics, lowered "
                         "for shaderc_msl's atomic64_texture pass (tools/speckle_fix.py)")
    ap.add_argument("--cs-implicit-lod", action="store_true",
                    help="translator-fork: implicit-LOD samples in compute take quad-derivative LODs plus bias")
    ap.add_argument("--mesh-index-clamp", action="store_true",
                    help="translator-fork: mesh primitive indices clamped below the vertex count; no vertices, no "
                         "primitives")


def configure(a):
    ON["atomic64"] = a.image_atomic64
    ON["cs_implicit_lod"] = a.cs_implicit_lod
    ON["mesh_index_clamp"] = a.mesh_index_clamp


def is_atomic64(op, vdata, imm_ops):
    return ON["atomic64"] and op in imm_ops and vdata.get("k") == "vgpr" and vdata.get("n", 1) == 2


def image_atomic64(g, op, a, mods, vdata, idx, iv, run, unsupported):
    """g: the generator (rdna2spirv Gen). Emits the marked write described in the module docstring."""
    if op not in ATOMIC64_MARK:
        raise unsupported(op + " 64-bit (only umax/umin are lowered)")
    if "glc" in mods:
        raise unsupported(op + " 64-bit glc (the old value is not returned by the Metal texture atomic)")
    lo = g.load_reg("vgpr", vdata["r"])
    hi = g.load_reg("vgpr", vdata["r"] + 1)
    found, fmt, is64 = g.tmp(), g.tmp(), g.tmp()
    g.emit(f"{found} = OpINotEqual %bool {idx} {g.cu(0)}")
    t1 = g.sgpr_word(a[2], 1)
    g.emit(f"{fmt} = OpBitFieldUExtract %uint {t1} {g.cu(20)} {g.cu(9)}")
    g.emit(f"{is64} = OpIEqual %bool {fmt} {g.cu(GFX10_FORMAT_32_32_UINT)}")
    act = g.emit_land(run, g.emit_land(found, is64))
    di = 0
    g.uses_store_views.add(di)
    for w in [16] + sum(g.int_format_masks(), []):
        g.cu(w)
    do, end = g.tmp() + "_a64do", g.tmp() + "_a64end"
    g.emit(f"OpSelectionMerge {end} None")
    g.emit(f"OpBranchConditional {act} {do} {end}")
    g.emit(f"{do} = OpLabel")
    ip, img, texel = g.tmp(), g.tmp(), g.tmp()
    g.dynamic_storage_access(ip, f"%_ptr_UC_stv1_{di}", f"%stv1_{di}", idx)
    g.emit(f"{img} = OpLoad %stimg1_{di} {ip}")
    g.emit(f"{texel} = OpCompositeConstruct %v4uint_st {lo} {hi} {g.cu(ATOMIC64_MARK[op])} {g.cu(ATOMIC64_MARK2)}")
    g.emit(f"OpImageWrite {img} {iv} {texel}")
    g.emit(f"OpBranch {end}")
    g.emit(f"{end} = OpLabel")
    g.speckle_atomic64 = getattr(g, "speckle_atomic64", 0) + 1


def mesh_epilogue(L, c):
    """--mesh-index-clamp on the hw mesh epilogue's lines: no vertices -> no primitives; indices below the count."""
    if not ON["mesh_index_clamp"]:
        return L
    out, done = [], set()
    for line in L:
        if line.startswith("%msp = OpExtInst %uint %glsl UMin %msp0 "):
            out += [line.replace("%msp = ", "%msp_u = ", 1),
                    "%msv_z = OpIEqual %bool %msv %c0",
                    "%msp = OpSelect %uint %msv_z %c0 %msp_u"]
            done.add("count")
        elif line == "%mix = OpCompositeConstruct %v3uint %mi0 %mi1 %mi2":
            out += [f"%msv_m1 = OpISub %uint %msv {c(1)}",
                    "%mi0c = OpExtInst %uint %glsl UMin %mi0 %msv_m1",
                    "%mi1c = OpExtInst %uint %glsl UMin %mi1 %msv_m1",
                    "%mi2c = OpExtInst %uint %glsl UMin %mi2 %msv_m1",
                    "%mix = OpCompositeConstruct %v3uint %mi0c %mi1c %mi2c"]
            done.add("clamp")
        else:
            out.append(line)
    if done != {"count", "clamp"}:
        raise RuntimeError("--mesh-index-clamp: the mesh epilogue changed shape (%s)" % sorted(done))
    return out


def cs_quad_grads(g, kind, f, grad_width, bias, dim):
    """--cs-implicit-lod: (dPdx, dPdy) of the coordinates over the console quad (lanes 4k..4k+3, x from lane 1 - lane
    0, y from lane 2 - lane 0, the same for all four lanes: a coarse derivative), scaled by 2^bias for _b / _c_b; None
    when the lowering does not apply (not compute, not an implicit form, a cube)."""
    if not ON["cs_implicit_lod"] or g.stage != "cs" or kind not in ("", "_b", "_c", "_c_b") or \
            dim == "dim:SQ_RSRC_IMG_CUBE":
        return None
    g.uses_quad = True
    vt = "%float" if grad_width == 1 else f"%v{grad_width}float"
    if grad_width == 1:
        p = f[0]
    else:
        p = g.tmp()
        g.emit(f"{p} = OpCompositeConstruct {vt} {' '.join(f[:grad_width])}")
    q = []
    for lane in (0, 1, 2):
        t = g.tmp()
        g.emit(f"{t} = OpGroupNonUniformQuadBroadcast {vt} {g.cu(3)} {p} {g.cu(lane)}")
        q.append(t)
    dx, dy = g.tmp(), g.tmp()
    g.emit(f"{dx} = OpFSub {vt} {q[1]} {q[0]}")
    g.emit(f"{dy} = OpFSub {vt} {q[2]} {q[0]}")
    if bias is not None and kind in ("_b", "_c_b"):
        s, sx, sy = g.tmp(), g.tmp(), g.tmp()
        g.emit(f"{s} = OpExtInst %float %glsl Exp2 {bias}")
        if grad_width == 1:
            g.emit(f"{sx} = OpFMul %float {dx} {s}")
            g.emit(f"{sy} = OpFMul %float {dy} {s}")
        else:
            g.emit(f"{sx} = OpVectorTimesScalar {vt} {dx} {s}")
            g.emit(f"{sy} = OpVectorTimesScalar {vt} {dy} {s}")
        dx, dy = sx, sy
    g.speckle_cs_lod = getattr(g, "speckle_cs_lod", 0) + 1
    return dx, dy


def quad_ok(g, line):
    """Under --wave64-emulate, a quad broadcast cs_quad_grads emitted: lanes 4k..4k+3 of a console wave are four lanes
    of one host SIMD group, and every lane of a wave runs the segment (EXEC is a predicate there), so it is exact."""
    return bool(getattr(g, "speckle_cs_lod", 0)) and "OpGroupNonUniformQuadBroadcast" in line
