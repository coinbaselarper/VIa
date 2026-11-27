#!/usr/bin/env python3
"""Speed work on translated mesh shaders: the store row's MSL (recipe + private mesh outputs, as the default overlay
loads it), instrumented variants for the offline probe (tools/mspeed_probe.mm), and metallib builds.

  mspeed.py msl NAME OUT.metal                    the shader as the title runs it (store recipe + private outputs)
  mspeed.py trace IN.metal OUT.metal              loads go through a tracer (buffer 29): word bitmap, out-of-range
                                                  count, hash-lookup keys, dispatch-loop counts, cases taken
  mspeed.py safe IN.metal OUT.metal               loads outside the probe heap (buffer 29: base, words) read 0
  mspeed.py capture IN.metal OUT.metal            outputs of every thread written to buffer 30 (A/B checks)
  mspeed.py lib IN.metal OUT.metallib             metal -c with the recipe's flags, metallib
"""
import gzip
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(PORT, "d3d12_metal", "tools"))

TRACE_HEAD = 65536
TRACE_KEYS = 1024
TRACE_CASES = 8192
TRACE_BITMAP = 16384 * 4


def match_paren(s, i):
    """Index just past the parenthesis that closes the one at s[i]."""
    depth = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise ValueError("unbalanced parentheses")


def match_brace(s, i):
    depth = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise ValueError("unbalanced braces")


FUNC = re.compile(r"^(?:static inline __attribute__\(\(always_inline\)\)\n)?(?:\[\[mesh\]\] )?[A-Za-z_][\w<>, :]*? (_\w+|main0)\(", re.M)


def functions(msl):
    """name -> (signature start, index of '(' , index past ')', body start '{', body end)."""
    out = {}
    for m in FUNC.finditer(msl):
        name = m.group(1)
        lp = m.end() - 1
        rp = match_paren(msl, lp)
        rest = msl[rp:rp + 3]
        if not rest.startswith("\n{"):
            continue
        bs = rp + 1
        be = match_brace(msl, bs)
        out[name] = (m.start(), lp, rp, bs, be)
    return out


def replace_loads(text, fn):
    """Rewrite every heap load of the translation with fn(address expression)."""
    pat1 = "(*(reinterpret_cast<const device spvUnsafeArray<uint, 1>*>("
    pat2 = "*(reinterpret_cast<const device uint*>("
    out, i, n = [], 0, 0
    while True:
        a = text.find(pat1, i)
        b = text.find(pat2, i)
        if a < 0 and b < 0:
            out.append(text[i:])
            break
        if a >= 0 and (b < 0 or a <= b):
            lp = a + len(pat1) - 1
            rp = match_paren(text, lp)
            if not text.startswith("))[", rp):
                raise ValueError("unexpected load form at %d" % a)
            k_end = text.index("]", rp + 3)
            k = text[rp + 3:k_end]
            out.append(text[i:a])
            out.append(fn("(%s) + ulong(%s) * 4ul" % (text[lp + 1:rp - 1], k)))
            i = k_end + 1
        else:
            lp = b + len(pat2) - 1
            rp = match_paren(text, lp)
            if not text.startswith(")", rp):
                raise ValueError("unexpected load form at %d" % b)
            out.append(text[i:b])
            out.append(fn(text[lp + 1:rp - 1]))
            i = rp + 1
        n += 1
    return "".join(out), n


def add_param(msl, names, decl, arg):
    """Add parameter `decl` to the definitions of `names` and `arg` to every call of them."""
    if not names:
        return msl
    for name in names:
        f = functions(msl)
        s, lp, rp, bs, be = f[name]
        msl = msl[:rp - 1] + ", " + decl + msl[rp - 1:]
    calls = re.compile(r"(?<![\w.])(" + "|".join(re.escape(n) for n in names) + r")\(")
    out, i = [], 0
    defs = {functions(msl)[n][1] for n in names}
    for m in calls.finditer(msl):
        lp = m.end() - 1
        if lp in defs or lp < i:
            continue
        rp = match_paren(msl, lp)
        out.append(msl[i:rp - 1])
        out.append(", " + arg + ")")
        i = rp
    out.append(msl[i:])
    return "".join(out)


def callers_closure(msl, seed):
    f = functions(msl)
    need = set(seed)
    changed = True
    while changed:
        changed = False
        for name, (s, lp, rp, bs, be) in f.items():
            if name in need or name == "main0":
                continue
            body = msl[bs:be]
            if any(re.search(r"(?<![\w.])" + re.escape(n) + r"\(", body) for n in need):
                need.add(name)
                changed = True
    return need


def program_function(msl):
    """The translated program's body function (the one with the dispatch switch) and its PC variable."""
    for name, (s, lp, rp, bs, be) in functions(msl).items():
        m = re.search(r"\n(\s*)switch \((_\d+)\)\n", msl[bs:be])
        if m:
            return name, m.group(2)
    raise ValueError("no dispatch switch")


TRACE_HELPER = r"""
static inline uint mspeed_ld(ulong a, uint site, device atomic_uint* t)
{
    ulong base = (ulong(atomic_load_explicit(&t[1], memory_order_relaxed)) << 32) | ulong(atomic_load_explicit(&t[0], memory_order_relaxed));
    ulong words = ulong(atomic_load_explicit(&t[2], memory_order_relaxed));
    if (a < base || ((a - base) >> 2) >= words || (a & 3ul) != 0ul)
    {
        uint s = atomic_fetch_add_explicit(&t[3], 1u, memory_order_relaxed);
        if (s < 128u)
        {
            atomic_store_explicit(&t[64 + 4 * s], uint(a - base), memory_order_relaxed);
            atomic_store_explicit(&t[65 + 4 * s], uint((a - base) >> 32), memory_order_relaxed);
            atomic_store_explicit(&t[66 + 4 * s], site, memory_order_relaxed);
        }
        return 0u;
    }
    uint w = uint((a - base) >> 2);
    atomic_fetch_or_explicit(&t[%(bitmap)d + (w >> 5)], 1u << (w & 31u), memory_order_relaxed);
    return *(reinterpret_cast<const device uint*>(a));
}
"""


def trace(msl):
    sites = []

    def ld(e):
        sites.append(e)
        return "mspeed_ld(%s, %du, _mtr)" % (e, len(sites) - 1)
    msl, n = replace_loads(msl, ld)
    msl = msl.replace("using namespace metal;\n", "using namespace metal;\n" + TRACE_HELPER % {"bitmap": TRACE_BITMAP}, 1)
    f = functions(msl)
    hashf = [nm for nm, (s, lp, rp, bs, be) in f.items() if "2166136261u" in msl[bs:be]]
    users = [nm for nm, (s, lp, rp, bs, be) in f.items() if nm != "main0" and "mspeed_ld(" in msl[bs:be]]
    prog, pc = program_function(msl)
    need = callers_closure(msl, set(users) | set(hashf) | {prog})
    msl = add_param(msl, sorted(need), "device atomic_uint* _mtr", "_mtr")
    for h in hashf:
        s, lp, rp, bs, be = functions(msl)[h]
        params = re.findall(r"uint (_\d+)", msl[lp:rp])[:8]
        rec = ["    if (simd_is_first())", "    {",
               "        uint ks = atomic_fetch_add_explicit(&_mtr[4], 1u, memory_order_relaxed);",
               "        if (ks < %du)" % ((TRACE_CASES - TRACE_KEYS) // 9), "        {"]
        for j, p in enumerate(params):
            rec.append("            atomic_store_explicit(&_mtr[%d + ks * 9u + %du], %s, memory_order_relaxed);" % (TRACE_KEYS, j, p))
        rec += ["        }", "    }"]
        msl = msl[:bs + 2] + "\n".join(rec) + "\n" + msl[bs + 2:]
    s, lp, rp, bs, be = functions(msl)[prog]
    body = msl[bs:be]
    m = re.search(r"\n(\s*)switch \(" + pc + r"\)\n", body)
    ind = m.group(1)
    ins = ("\n%sif (simd_is_first()) { atomic_fetch_add_explicit(&_mtr[5], 1u, memory_order_relaxed); "
           "atomic_fetch_or_explicit(&_mtr[%d + (%s >> 5u)], 1u << (%s & 31u), memory_order_relaxed); }"
           "\n%sif (gl_LocalInvocationIndex == 0u || gl_LocalInvocationIndex == 64u) { uint q = atomic_fetch_add_explicit(&_mtr[7], 1u, memory_order_relaxed); "
           "if (q < 49152u) atomic_store_explicit(&_mtr[16384u + q], %s | (gl_LocalInvocationIndex << 10u) | (atomic_load_explicit(&_mtr[6], memory_order_relaxed) << 16u), memory_order_relaxed); }"
           % (ind, TRACE_CASES, pc, pc, ind, pc))
    body = body[:m.start()] + ins + body[m.start():]
    m = re.search(r"\n    for \(;;\)\n    \{\n", body)
    if m:
        body = body[:m.end()] + "        if (simd_is_first()) atomic_fetch_add_explicit(&_mtr[6], 1u, memory_order_relaxed);\n" + body[m.end():]
    msl = msl[:bs] + body + msl[be:]
    s, lp, rp, bs, be = functions(msl)["main0"]
    msl = msl[:rp - 1] + ", device atomic_uint* _mtr [[buffer(29)]]" + msl[rp - 1:]
    return msl, n


SAFE_HELPER = r"""
static inline uint mspeed_lds(ulong a, const device uint* t)
{
    ulong base = (ulong(t[1]) << 32) | ulong(t[0]);
    if (a < base || ((a - base) >> 2) >= ulong(t[2]) || (a & 3ul) != 0ul)
    {
        return 0u;
    }
    return *(reinterpret_cast<const device uint*>(a));
}
"""


def safe(msl):
    """Loads outside the probe's heap read 0 (synthetic inputs may name any address); buffer 29 holds the heap range."""
    msl, n = replace_loads(msl, lambda e: "mspeed_lds(%s, _mtr)" % e)
    msl = msl.replace("using namespace metal;\n", "using namespace metal;\n" + SAFE_HELPER, 1)
    f = functions(msl)
    users = [nm for nm, (s, lp, rp, bs, be) in f.items() if nm != "main0" and "mspeed_lds(" in msl[bs:be]]
    need = callers_closure(msl, set(users))
    msl = add_param(msl, sorted(need), "const device uint* _mtr", "_mtr")
    s, lp, rp, bs, be = functions(msl)["main0"]
    msl = msl[:rp - 1] + ", const device uint* _mtr [[buffer(29)]]" + msl[rp - 1:]
    return msl, n


def capture(msl):
    """Every thread's outputs after the program ran: buffer 30, per group 129 records of R words (record 0: the mesh
    sizes), thread t at record 1 + t: position, each user attribute, primitive indices, cull flag."""
    s, lp, rp, bs, be = functions(msl)["main0"]
    body = msl[bs:be]
    st = re.search(r"struct spvPerVertex\n\{\n(.*?)\n\};", msl, re.S).group(1)
    attrs = re.findall(r"float4 m_(\d+) \[\[user\(locn(\d+)\)\]\];", st)
    rec = 4 + 4 * len(attrs) + 4
    lines = ["    {", "        device uint* mo = _mout + (gl_WorkGroupID.x * 129u) * %du;" % rec,
             "        if (gl_LocalInvocationIndex == 0) { mo[0] = spvMeshSizes.x; mo[1] = spvMeshSizes.y; mo[2] = 0x5eed5eedu; }",
             "        device uint* r = mo + (1u + gl_LocalInvocationIndex) * %du;" % rec,
             "        r[0] = as_type<uint>(gl_Position.x); r[1] = as_type<uint>(gl_Position.y); r[2] = as_type<uint>(gl_Position.z); r[3] = as_type<uint>(gl_Position.w);"]
    for j, (m, loc) in enumerate(attrs):
        o = 4 + 4 * j
        lines.append("        r[%d] = as_type<uint>(_%s.x); r[%d] = as_type<uint>(_%s.y); r[%d] = as_type<uint>(_%s.z); r[%d] = as_type<uint>(_%s.w);"
                     % (o, m, o + 1, m, o + 2, m, o + 3, m))
    o = 4 + 4 * len(attrs)
    lines.append("        r[%d] = gl_PrimitiveTriangleIndicesEXT.x; r[%d] = gl_PrimitiveTriangleIndicesEXT.y; r[%d] = gl_PrimitiveTriangleIndicesEXT.z; r[%d] = uint(gl_MeshPrimitivesEXT.gl_CullPrimitiveEXT);"
                 % (o, o + 1, o + 2, o + 3))
    lines.append("    }")
    anchor = "    threadgroup_barrier(mem_flags::mem_threadgroup);\n    if (spvMeshSizes.y == 0)"
    if anchor not in body:
        raise ValueError("capture: no copy section")
    body = body.replace(anchor, "    threadgroup_barrier(mem_flags::mem_threadgroup);\n" + "\n".join(lines) + "\n    if (spvMeshSizes.y == 0)", 1)
    msl = msl[:bs] + body + msl[be:]
    s, lp, rp, bs, be = functions(msl)["main0"]
    msl = msl[:rp - 1] + ", device uint* _mout [[buffer(30)]]" + msl[rp - 1:]
    return msl, {"record_words": rec, "attributes": [int(l) for _, l in attrs]}


def base_msl(name):
    import mesh_tg_pack as P
    import mesh_out_private as MOP
    head, rows = P.manifest_rows()
    row = rows[name]
    tmp = os.path.join(PORT, "build", "mspeed-x86_64", "tmp", str(os.getpid()))
    msl, passes, base = P.cross(name, row, tmp)
    if msl is None:
        raise RuntimeError("spirv-cross failed for " + name)
    new, arrays, why = MOP.privatize(msl)
    return (new if new is not None else msl), [p["pass"] for p in passes] + (["private_mesh_outputs"] if new else [])


def build_lib(src, out):
    import shaderc_msl as R
    air = out + ".air"
    rc, o, e, dt = R.run([R.tool("metal"), "-std=" + R.RECIPE["std"], *R.RECIPE["metal"], src, "-o", air], R.RECIPE["metal_timeout"])
    if rc != 0:
        raise RuntimeError("metal failed: " + (e or b"").decode("utf-8", "replace")[-1500:])
    rc, o, e, dt2 = R.run([R.tool("metallib"), air, "-o", out], R.RECIPE["metal_timeout"])
    os.remove(air)
    if rc != 0:
        raise RuntimeError("metallib failed")
    return dt



def count(msl):
    """GTA6RT_METAL_GPU_DEBUG variant: debug-buffer words 128 + groups, 129 + rounds, 130 + segments of wave 0, 131 +
    vertices, 132 + segments of wave 1 (printed as the mesh debug-variant line of the work report)."""
    prog, pc = program_function(msl)
    s, lp, rp, bs, be = functions(msl)[prog]
    body = msl[bs:be]
    m = re.search(r"\n(\s*)switch \(" + pc + r"\)\n", body)
    ind = m.group(1)
    body = (body[:m.start()] + "\n%sif (gl_LocalInvocationIndex == 0u) _mseg0++; else if (gl_LocalInvocationIndex == 64u) _mseg1++;" % ind
            + body[m.start():])
    m = re.search(r"\n    for \(;;\)\n    \{\n", body)
    body = body[:m.end()] + "        _mround++;\n" + body[m.end():]
    first = body.index("{") + 1
    body = body[:first] + "\n    uint _mseg0 = 0u, _mseg1 = 0u, _mround = 0u;" + body[first:]
    end = body.rindex("}")
    body = (body[:end] + "    if (gl_LocalInvocationIndex == 0u)\n    {\n"
            "        atomic_fetch_add_explicit(&_mdbg[128], 1u, memory_order_relaxed);\n"
            "        atomic_fetch_add_explicit(&_mdbg[129], _mround, memory_order_relaxed);\n"
            "        atomic_fetch_add_explicit(&_mdbg[130], _mseg0, memory_order_relaxed);\n"
            "        atomic_fetch_add_explicit(&_mdbg[131], min((_6 & 4095u), 128u), memory_order_relaxed);\n    }\n"
            "    if (gl_LocalInvocationIndex == 64u)\n    {\n"
            "        atomic_fetch_add_explicit(&_mdbg[132], _mseg1, memory_order_relaxed);\n    }\n" + body[end:])
    msl = msl[:bs] + body + msl[be:]
    msl = add_param(msl, [prog], "device atomic_uint* _mdbg", "_mdbg")
    s, lp, rp, bs, be = functions(msl)["main0"]
    msl = msl[:rp - 1] + ", device atomic_uint* _mdbg [[buffer(30)]]" + msl[rp - 1:]
    return msl


def main():
    cmd = sys.argv[1]
    if cmd == "msl":
        msl, passes = base_msl(sys.argv[2])
        open(sys.argv[3], "w").write(msl)
        print("passes", passes)
    elif cmd == "trace":
        msl, n = trace(open(sys.argv[2]).read())
        open(sys.argv[3], "w").write(msl)
        print("loads traced", n)
    elif cmd == "safe":
        msl, n = safe(open(sys.argv[2]).read())
        open(sys.argv[3], "w").write(msl)
        print("loads bounded", n)
    elif cmd == "capture":
        msl, info = capture(open(sys.argv[2]).read())
        open(sys.argv[3], "w").write(msl)
        print(info)
    elif cmd == "count":
        open(sys.argv[3], "w").write(count(open(sys.argv[2]).read()))
    elif cmd == "lib":
        print("compiled in %.1f s" % build_lib(sys.argv[2], sys.argv[3]))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()

