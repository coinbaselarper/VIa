#!/usr/bin/env python3
"""shaderc_msl recipe: one translated SPIR-V shader -> Metal library, content-addressed.

Recipe (version in RECIPE["version"]):
  1. SPIR-V pre-passes (tools/spv.py):
     split_bindings     always: set-0 binding b -> set 1 + b (one argument buffer per contract binding)
     lower_vertex_lane  vs whose code loads SubgroupLocalInvocationId, when vs_route == "lane"
     drop_outputs       ms (or vs) with a dead-output list from tools/mesh_fit.py
  2. spirv-cross --msl --msl-version 30100 --msl-argument-buffers --msl-argument-buffer-tier 1
                 --msl-device-argument-buffer 1..29 [--msl-vertex-for-tessellation when vs_route == "kernel"]
  3. metal -c -std=metal3.1 (timeout, then one -O0 retry), metallib
Output: <cas>/<key[:2]>/<key>.metallib and <key>.json (the result row). The key hashes the SPIR-V bytes, the
translator fingerprint, the recipe, the tool versions and the pre-pass parameters, so any change gives a new key
and old outputs stay valid for their own inputs.
"""
import gzip, hashlib, json, os, re, shutil, subprocess, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spv  # noqa: E402

RECIPE = {
    "name": "shaderc_msl",
    "version": 1,
    "msl_version": 30100,
    "std": "metal3.1",
    "cross": ["--msl", "--msl-argument-buffers", "--msl-argument-buffer-tier", "1"]
             + sum((["--msl-device-argument-buffer", str(s)] for s in range(1, 30)), []),
    "metal": ["-c", "-w"],
    "cross_timeout": 300,
    "metal_timeout": 600,
    "o0_fallback": True,
}
SPIRV_CROSS = os.environ.get("SHADERC_SPIRV_CROSS", "/opt/homebrew/bin/spirv-cross")
_TOOLS = {}


def tool(name):
    if name not in _TOOLS:
        _TOOLS[name] = subprocess.run(["xcrun", "-f", name], capture_output=True, text=True, check=True).stdout.strip()
    return _TOOLS[name]


def tool_versions():
    if "versions" not in _TOOLS:
        sc = subprocess.run([SPIRV_CROSS, "--revision"], capture_output=True, text=True).stdout.strip()
        mv = subprocess.run([tool("metal"), "--version"], capture_output=True, text=True).stdout.splitlines()[0]
        brew = os.path.realpath(SPIRV_CROSS)
        _TOOLS["versions"] = {"spirv_cross": sc, "spirv_cross_path": brew, "metal": mv}
    return _TOOLS["versions"]


def translator_id(spirv_dir):
    try:
        sh = json.load(open(os.path.join(spirv_dir, "shards.json")))
        src = open(os.path.join(spirv_dir, "translation_sources.sha256")).read().strip()
        return {"translator": sh.get("translator"), "sources": src, "fallbacks": sh.get("fallbacks"),
                "wave64_emulate": sh.get("wave64_emulate")}
    except OSError:
        return {"translator": None}


PEAK = {"rss": 0}


def run(cmd, timeout):
    """Run cmd; returns (rc or None on timeout, stdout, stderr, seconds). Child peak RSS goes to PEAK["last"]."""
    import tempfile
    t0 = time.monotonic()
    with tempfile.TemporaryFile() as fo, tempfile.TemporaryFile() as fe:
        p = subprocess.Popen(cmd, stdout=fo, stderr=fe, start_new_session=True)
        rc, ru = None, None
        delay = 0.002
        while True:
            pid, status, ru = os.wait4(p.pid, os.WNOHANG)
            if pid:
                rc = os.waitstatus_to_exitcode(status)
                break
            if time.monotonic() - t0 > timeout:
                os.killpg(p.pid, 9)
                _, status, ru = os.wait4(p.pid, 0)
                rc = None
                break
            time.sleep(delay)
            delay = min(delay * 1.5, 0.05)
        p.returncode = rc if rc is not None else -9
        fo.seek(0)
        fe.seek(0)
        out, err = fo.read(), fe.read()
    PEAK["last"] = ru.ru_maxrss if ru else 0
    PEAK["rss"] = max(PEAK["rss"], PEAK["last"])
    if rc is None:
        err = b"TIMEOUT"
    return rc, out, err, time.monotonic() - t0


def loads_var(words, var):
    for i, op, n in spv.insts(words):
        if op == spv.OP_LOAD and words[i + 3] == var:
            return True
    return False


def vs_lane_used(words):
    m = spv.Module(words)
    if m.model != "vs":
        return False
    for v, (_, sc, _) in m.vars.items():
        if sc == spv.SC_INPUT and m.dec(v, spv.DEC_BUILTIN) in (spv.BI_SUBGROUP_LOCAL_ID, spv.BI_SUBGROUP_SIZE):
            if loads_var(words, v):
                return True
    return False


def prepare(words, stage, opts):
    passes = []
    if opts.get("x_buf_device"):
        import cs_xbuf
        words, info = cs_xbuf.x_buf_to_device(words)
        passes.append({"pass": "x_buf_device", "x_buf_words": info["x_buf_words"], "binding": info["binding"]})
    w, r = spv.split_bindings(words)
    passes.append({"pass": "split_bindings", "moved": r["moved"]})
    if opts.get("dual_source") and stage == "ps":
        w, r = spv.dual_source(w)
        if r["moved"]:
            passes.append({"pass": "dual_source"})
    if opts.get("output_variants") and stage == "ps":
        import out_types
        info = out_types.analyze(w, opts.get("output_signature"))
        codes = sorted({out_types.normalize(c).rstrip("f") for c in opts["output_variants"]} - {""})
        codes = [c for c in codes if out_types.needs_variant(info, c)]
        if codes:
            passes.append({"pass": "output_variants", "codes": codes, "version": out_types.VERSION})
    extra = []
    if stage == "vs" and vs_lane_used(w):
        route = opts.get("vs_route", "lane")
        if route == "lane":
            w, r = spv.lower_vertex_lane(w)
            passes.append({"pass": "lower_vertex_lane"})
        else:
            extra = ["--msl-vertex-for-tessellation"]
            passes.append({"pass": "vertex_for_tessellation"})
    drop = opts.get("drop_outputs")
    if drop:
        w, r = spv.drop_outputs(w, drop)
        passes.append({"pass": "drop_outputs", "locations": r["dropped"], "kept_private": r["kept_private"]})
    if opts.get("shrink_lds"):
        w, r = spv.shrink_lds(w, int(opts["shrink_lds"]))
        passes.append({"pass": "shrink_lds", "dwords": int(opts["shrink_lds"]), "applied": r["applied"]})
    if len(w) * 4 > NOINLINE_MIN_SPV_BYTES:
        hot = hot_callees(w, NOINLINE_MIN_CALLS)
        if hot:
            passes.append({"pass": "noinline_hot_helpers", "level": "msl", "min_calls": NOINLINE_MIN_CALLS,
                           "callees": len(hot)})
    caps = spv.Module(w).caps
    if caps & {10}:
        passes.append({"pass": "soft_f64", "level": "msl"})
    if caps & {5378} and stage == "ps":
        passes.append({"pass": "argbuf_raster_order_strip", "level": "msl"})
    if caps & {33} and stage == "vs":
        passes.append({"pass": "cull_distance_local", "level": "msl"})
    trim = opts.get("trim_outputs")
    if trim:
        w, r = spv.trim_outputs(w, {int(k): v for k, v in trim.items()})
        passes.append({"pass": "trim_outputs", "widths": {str(k): v for k, v in sorted(r["trimmed"].items())}})
        passes.append({"pass": "pack_mesh_struct", "level": "msl"})
    return w, passes, extra


def apply_split(w, passes, opts):
    """Meshlet split (tools/mesh_split.py): returns (mesh words, task words or None). Adds its pass entries."""
    sp = opts.get("split_mesh")
    if not sp:
        return w, None
    import mesh_split
    mesh, task, rep = mesh_split.split_mesh(w, int(sp["chunks"]), int(sp["vmax"]), int(sp["pmax"]))
    passes.append({"pass": "split_mesh", "chunks": rep["chunks"], "vmax": rep["vmax"], "pmax": rep["pmax"],
                   "version": mesh_split.VERSION})
    if not any(p["pass"] == "pack_mesh_struct" for p in passes):
        passes.append({"pass": "pack_mesh_struct", "level": "msl"})
    return mesh, task


def compile_task(task_words, cas, tmp):
    """Object-stage library of a split mesh shader; one library per chunk count, shared by every shader."""
    blob = spv.to_bytes(task_words)
    key = hashlib.sha256(json.dumps({"spv": hashlib.sha256(blob).hexdigest(), "recipe": RECIPE,
                                     "tools": tool_versions()}, sort_keys=True).encode()).hexdigest()
    d = os.path.join(cas, key[:2])
    os.makedirs(d, exist_ok=True)
    lib = os.path.join(d, key + ".metallib")
    if os.path.exists(lib):
        return key
    base = os.path.join(tmp, "task_" + key)
    open(base + ".spv", "wb").write(blob)
    rc, out, err, dt = run([SPIRV_CROSS, base + ".spv", *RECIPE["cross"], "--msl-version", str(RECIPE["msl_version"]),
                            "--output", base + ".metal"], RECIPE["cross_timeout"])
    if rc != 0:
        raise RuntimeError("task spirv-cross: " + err.decode("utf-8", "replace")[:300])
    rc, out, err, dt = run([tool("metal"), "-std=" + RECIPE["std"], *RECIPE["metal"], base + ".metal", "-o", base + ".air"],
                           RECIPE["metal_timeout"])
    if rc != 0:
        raise RuntimeError("task metal: " + err.decode("utf-8", "replace")[:300])
    part = "%s.%d.part" % (lib, os.getpid())
    rc, out, err, dt = run([tool("metallib"), base + ".air", "-o", part], RECIPE["metal_timeout"])
    if rc != 0:
        raise RuntimeError("task metallib: " + err.decode("utf-8", "replace")[:300])
    os.replace(part, lib)
    for ext in (".spv", ".metal", ".air"):
        try:
            os.remove(base + ext)
        except OSError:
            pass
    return key


NOINLINE_MIN_SPV_BYTES = 8 << 20
NOINLINE_MIN_CALLS = 100


NOINLINE_MIN_INLINED_WORDS = 1_000_000


def hot_callees(words, min_calls):
    """Callees with at least min_calls call sites, if inlining them would add NOINLINE_MIN_INLINED_WORDS or more."""
    calls, size, cur, start = {}, {}, None, 0
    for i, op, n in spv.insts(words):
        if op == 57:
            calls[words[i + 3]] = calls.get(words[i + 3], 0) + 1
        elif op == spv.OP_FUNCTION:
            cur, start = words[i + 2], i
        elif op == 56 and cur is not None:
            size[cur] = i - start
            cur = None
    hot = sorted(f for f, c in calls.items() if c >= min_calls)
    return hot if sum(calls[f] * size.get(f, 0) for f in hot) >= NOINLINE_MIN_INLINED_WORDS else []


def noinline_hot(msl, min_calls):
    """SPIRV-Cross prints every helper as static inline always_inline; for helpers called at least min_calls times
    print static noinline instead (measured on the huge compute shaders: STATUS.md section 5)."""
    out, n = msl, 0
    for h in re.finditer(r"^static inline __attribute__\(\(always_inline\)\)\n[^\n]*?\b(_\d+)\(", msl, re.M):
        if len(re.findall(r"\b" + h.group(1) + r"\(", msl)) - 1 >= min_calls:
            out = out.replace(h.group(0), h.group(0).replace("static inline __attribute__((always_inline))",
                                                             "static __attribute__((noinline))"), 1)
            n += 1
    return out, n


def lds_allocation(dxbc_path):
    """RSRC2 LDS_SIZE of the FSSM program inside a carved DXBC (XBCH record, zlib), in bytes; None if absent."""
    import zlib
    try:
        b = open(dxbc_path, "rb").read()
    except OSError:
        return None
    i = b.find(b"XBCH")
    if i < 0:
        return None
    ver, csize = spv.struct.unpack_from("<2I", b, i + 4)
    f = zlib.decompress(b[i + 24:i + 24 + csize])
    if f[:4] != b"FSSM":
        return None
    rsrc2 = spv.struct.unpack_from("<I", f, 0x138)[0]
    return ((rsrc2 >> 15) & 0x1FF) * 512


def cache_key(spv_bytes, translator, passes, extra):
    k = {"spv": hashlib.sha256(spv_bytes).hexdigest(), "translator": translator, "recipe": RECIPE,
         "tools": tool_versions(), "passes": passes, "extra": extra}
    return hashlib.sha256(json.dumps(k, sort_keys=True).encode()).hexdigest()


ENTRY_RE = re.compile(r"^(vertex|fragment|kernel|\[\[mesh\]\]) [^\n]*?\bmain0\((.*)\)$", re.M)


def split_params(s):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "(<[":
            depth += 1
        elif ch in ")>]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def struct_body(msl, name):
    m = re.search(r"^struct " + re.escape(name) + r"\n\{\n(.*?)^\};", msl, re.S | re.M)
    return m.group(1) if m else None


def msl_plan(msl):
    """Binding plan read from the generated MSL entry point (checked against Metal reflection by verify_bindings)."""
    m = ENTRY_RE.search(msl)
    if not m:
        return {"entry": None}
    plan = {"entry": m.group(1).strip("[]"), "sets": {}, "discrete": {}, "push": None, "other_buffers": [], "textures": [],
            "samplers": []}
    for p in split_params(m.group(2)):
        a = re.search(r"\[\[buffer\((\d+)\)\]\]", p)
        if a:
            idx = int(a.group(1))
            s = re.search(r"\bspvDescriptorSet(\d+)\b", p)
            dd = re.search(r"\bspvDescriptorSet(\d+)Binding0\b", p)
            if dd:
                plan["discrete"][int(dd.group(1))] = idx
            elif s:
                plan["sets"][int(s.group(1))] = idx
            elif re.match(r"constant \w+& \w+ \[\[buffer", p):
                plan["push"] = idx
                plan["push_type"] = p.split("&")[0].split()[-1]
            else:
                plan["other_buffers"].append({"param": p, "index": idx})
        elif "[[texture(" in p:
            plan["textures"].append(p)
        elif "[[sampler(" in p:
            plan["samplers"].append(p)
    feats = {}
    mm = re.search(r"mesh<(\w+), (\w+), (\d+), (\d+), topology::(\w+)>", msl)
    if mm:
        feats["mesh_decl"] = [int(mm.group(3)), int(mm.group(4)), mm.group(5)]
    out = struct_body(msl, "main0_out")
    if out and plan["entry"] == "fragment":
        feats["colors"] = [(int(c.group(2)), c.group(1)) for c in re.finditer(r"(\w+) \w+ \[\[color\((\d+)\)\]\]", out)]
        feats["depth"] = "[[depth(" in out
        feats["stencil"] = "[[stencil]]" in out
    inp = struct_body(msl, "main0_in")
    if inp is not None and plan["entry"] == "fragment":
        feats["main0_in"] = inp
    feats["double"] = re.search(r"\bdouble\b", msl) is not None
    plan["features"] = feats
    return plan


def _align(line):
    m = re.match(r"\s*(\w+?)(\d?) \w+ \[\[[^\]]*\]\](?: \[\d+\])?;", line)
    if not m:
        return None
    base, n = m.group(1), int(m.group(2) or 1)
    if " [" in line.split("]]")[-1]:
        return 4
    size = 2 if base in ("half", "short", "ushort") else (1 if base in ("bool", "char", "uchar") else 4)
    return size * (4 if n == 3 else n)


def pack_mesh_struct(msl):
    """Sort the members of the mesh vertex struct by alignment (members are accessed by name only)."""
    m = re.search(r"\bmesh<(\w+), (\w+), \d+, \d+, topology::\w+>", msl)
    if not m:
        return msl, False
    name = m.group(1)
    if re.search(r"\b" + re.escape(name) + r"\s*\{", msl.split("struct " + name, 1)[-1].split("};", 1)[-1]):
        return msl, False
    body = struct_body(msl, name)
    lines = [l for l in body.splitlines() if l.strip()]
    keys = [_align(l) for l in lines]
    if None in keys:
        return msl, False
    order = [l for _, l in sorted(zip(keys, lines), key=lambda kl: -kl[0])]
    if order == lines:
        return msl, False
    new = "\n".join(order) + "\n"
    return msl.replace("struct " + name + "\n{\n" + body + "};", "struct " + name + "\n{\n" + new + "};", 1), True


SOFT_F64 = os.path.join(os.path.dirname(os.path.abspath(__file__)), "soft_f64.metal")


def soft_f64(msl):
    """Replace MSL double (Metal has none) by spvSoftF64: IEEE binary64 add/sub/compare in 64-bit integer code."""
    if not re.search(r"\bdouble\b", msl):
        return msl, False
    t = re.sub(r"\bdouble\b", "spvSoftF64", msl)
    t = t.replace("as_type<spvSoftF64>(", "spvSoftF64_from(").replace("as_type<uint2>(", "spv_as_uint2(")
    pre = open(SOFT_F64).read()
    t = t.replace("using namespace metal;\n", "using namespace metal;\n\n" + pre + "\n", 1)
    return t, True


def msl_fixups(text, passes):
    """MSL-level steps of the recipe, in order; returns (new text or None when unchanged, report)."""
    names = {p["pass"] for p in passes}
    info, changed = {}, False
    if "cull_distance_local" in names and "out.gl_CullDistance[" in text and re.search(r"> gl_CullDistance = \{\};", text):
        text = text.replace("out.gl_CullDistance[", "gl_CullDistance[")
        info["cull_distance_local"] = changed = True
    if "argbuf_raster_order_strip" in names:
        t2, n = re.subn(r"(\[\[id\(\d+\)), raster_order_group\(\d+\)\]\]", r"\1]]", text)
        info["raster_order_groups_stripped"] = n
        if n:
            text, changed = t2, True
    if "soft_f64" in names:
        text, c = soft_f64(text)
        info["soft_f64"] = c
        changed |= c
    if "noinline_hot_helpers" in names:
        text, c = noinline_hot(text, NOINLINE_MIN_CALLS)
        info["noinline_helpers"] = c
        changed |= bool(c)
    if "pack_mesh_struct" in names:
        text, c = pack_mesh_struct(text)
        info["mesh_struct_sorted"] = c
        changed |= c
    return (text if changed else None), info


def dxbc_of(spv_path):
    """The carved DXBC of a corpus shader: <shaders>/dxbc/X.dxbc for <shaders>/spirv/X.spv, <shaders>/D/X.dxbc for
    a sub-corpus shader <shaders>/spirv/D/X.spv."""
    d = os.path.dirname(os.path.abspath(spv_path))
    stem = os.path.basename(spv_path)[:-4]
    if os.path.basename(d) == "spirv":
        return os.path.join(os.path.dirname(d), "dxbc", stem + ".dxbc")
    return os.path.join(os.path.dirname(os.path.dirname(d)), os.path.basename(d), stem + ".dxbc")


def declared_output_variants(spv_path, opts):
    """Pixel shaders: the output-type variant the HLSL signature asks for (an integer SV_Target, read from the
    shader's DXBC beside the SPIR-V corpus) joins opts["output_variants"] (tools/out_types.py)."""
    import out_types
    opts = dict(opts)
    if "output_signature" not in opts:
        opts["output_signature"] = out_types.dxbc_targets(dxbc_of(spv_path))
    sig = opts["output_signature"] or {}
    code = "".join({"uint": "u", "int": "i"}.get(sig.get(loc), "f") for loc in range(max(sig) + 1 if sig else 0))
    if code.rstrip("f"):
        opts["output_variants"] = sorted(set(opts.get("output_variants") or []) | {code.rstrip("f")})
    return opts


def compile_one(spv_path, stage, cas, translator, opts=None, tmp=None, keep_msl=False):
    opts = opts or {}
    if stage == "ps" and opts.get("declared_output_variants", True):
        opts = declared_output_variants(spv_path, opts)
    t_start = time.monotonic()
    raw = open(spv_path, "rb").read()
    words = list(spv.struct.unpack("<%dI" % (len(raw) // 4), raw))
    t0 = time.monotonic()
    w, passes, extra = prepare(words, stage, opts)
    w, task_w = apply_split(w, passes, opts)
    pre_s = time.monotonic() - t0
    blob = spv.to_bytes(w)
    key = cache_key(raw, translator, passes, extra)
    d = os.path.join(cas, key[:2])
    os.makedirs(d, exist_ok=True)
    meta_path = os.path.join(d, key + ".json")
    if task_w is not None:
        tmp0 = tmp or os.path.join(cas, "..", "tmp", str(os.getpid()))
        os.makedirs(tmp0, exist_ok=True)
        object_key = compile_task(task_w, cas, tmp0)
    else:
        object_key = None
    if os.path.exists(meta_path) and not opts.get("force"):
        r = json.load(open(meta_path))
        r["cached"] = True
        return r
    tmp = tmp or os.path.join(cas, "..", "tmp", str(os.getpid()))
    os.makedirs(tmp, exist_ok=True)
    base = os.path.join(tmp, key)
    r = {"key": key, "spv": os.path.basename(spv_path), "stage": stage, "spv_bytes": len(raw), "passes": passes,
         "cross_extra": extra, "pre_s": round(pre_s, 4), "recipe_version": RECIPE["version"]}
    if object_key:
        r["object_key"] = object_key
        r["split_chunks"] = int(opts["split_mesh"]["chunks"])
    open(base + ".spv", "wb").write(blob)
    rc, out, err, dt = run([SPIRV_CROSS, base + ".spv", *RECIPE["cross"], "--msl-version", str(RECIPE["msl_version"]), *extra,
                            "--output", base + ".metal"], RECIPE["cross_timeout"])
    r["cross_s"] = round(dt, 3)
    r["cross_rss_mb"] = round(PEAK["last"] / 2**20, 1)
    if rc != 0 or not os.path.exists(base + ".metal"):
        r["status"] = "cross_fail"
        r["error"] = ("TIMEOUT" if rc is None else err.decode("utf-8", "replace").strip())[:800]
        return _finish(r, meta_path, base, t_start)
    msl = open(base + ".metal", "rb").read()
    r["msl_bytes"] = len(msl)
    text = msl.decode("utf-8", "replace")
    text, info = msl_fixups(text, passes)
    r.update(info)
    if text is not None:
        msl = text.encode()
        open(base + ".metal", "wb").write(msl)
    else:
        text = msl.decode("utf-8", "replace")
    r["plan"] = msl_plan(text)
    if any(p["pass"] == "x_buf_device" for p in passes):
        pl = r["plan"]
        if 30 in pl["sets"]:
            pl["x_buf_buffer"] = pl["sets"][30]
        else:
            ob = [b for b in pl["other_buffers"] if b["param"].startswith("device ") and "spvDescriptor" not in b["param"]]
            if len(ob) != 1:
                raise RuntimeError("x_buf_device: cannot find the exchange buffer parameter")
            pl["x_buf_buffer"] = ob[0]["index"]
    if keep_msl:
        with gzip.open(os.path.join(d, key + ".metal.gz"), "wb", compresslevel=1) as g:
            g.write(msl)
    air = base + ".air"
    airs = [air]
    for ov in (p for p in passes if p["pass"] == "output_variants"):
        r["output_variants"] = {}
        for c in ov["codes"]:
            vr = output_variant_air(w, c, base, passes, extra, r["plan"])
            r["output_variants"][c] = vr
            if vr["status"] != "ok":
                r["status"] = "variant_fail"
                r["error"] = "output variant %s: %s" % (c, vr.get("error", ""))
                return _finish(r, meta_path, base, t_start)
            airs.append(vr.pop("air"))
    rc, out, err, dt = run([tool("metal"), "-std=" + RECIPE["std"], *RECIPE["metal"], base + ".metal", "-o", air],
                           RECIPE["metal_timeout"])
    r["metal_s"] = round(dt, 3)
    r["metal_rss_mb"] = round(PEAK["last"] / 2**20, 1)
    r["opt"] = "default"
    if rc is None and RECIPE["o0_fallback"]:
        rc, out, err, dt = run([tool("metal"), "-std=" + RECIPE["std"], *RECIPE["metal"], "-O0", base + ".metal", "-o", air],
                               RECIPE["metal_timeout"])
        r["metal_o0_s"] = round(dt, 3)
        r["opt"] = "O0"
    if rc is not None and rc < 0:
        r["status"] = "killed"
        r["error"] = f"metal killed by signal {-rc}"
        return r
    if rc != 0:
        r["status"] = "metal_fail"
        e = err.decode("utf-8", "replace")
        r["error"] = "TIMEOUT" if rc is None else next((l.strip() for l in e.splitlines() if "error:" in l), e.strip())[:800]
        if not keep_msl:
            with gzip.open(os.path.join(d, key + ".metal.gz"), "wb", compresslevel=1) as g:
                g.write(msl)
        return _finish(r, meta_path, base, t_start)
    lib = os.path.join(d, key + ".metallib")
    part = "%s.%d.%d.part" % (lib, os.getpid(), threading.get_ident())
    rc, out, err, dt = run([tool("metallib"), *airs, "-o", part], RECIPE["metal_timeout"])
    r["metallib_s"] = round(dt, 3)
    if rc != 0:
        r["status"] = "metallib_fail"
        r["error"] = err.decode("utf-8", "replace").strip()[:800]
        return _finish(r, meta_path, base, t_start)
    os.replace(part, lib)
    r["metallib_bytes"] = os.path.getsize(lib)
    r["status"] = "ok"
    return _finish(r, meta_path, base, t_start)


def output_variant_air(w, code, base, passes, extra, plan):
    """One output-type variant (tools/out_types.py) of the prepared pixel shader `w`, compiled to AIR with the entry
    main0_o<code>; its binding plan must equal the base entry's."""
    import out_types
    entry = "main0_o" + code
    vw, rep = out_types.output_types(w, code)
    vr = {"entry": entry, "report": rep}
    if not rep["applied"]:
        vr.update(status="not_applied", error=rep.get("why", ""))
        return vr
    vb = base + "_o" + code
    open(vb + ".spv", "wb").write(spv.to_bytes(vw))
    rc, out, err, dt = run([SPIRV_CROSS, vb + ".spv", *RECIPE["cross"], "--msl-version", str(RECIPE["msl_version"]), *extra,
                            "--rename-entry-point", "main", entry, "frag", "--output", vb + ".metal"], RECIPE["cross_timeout"])
    if rc != 0 or not os.path.exists(vb + ".metal"):
        vr.update(status="cross_fail", error=("TIMEOUT" if rc is None else err.decode("utf-8", "replace").strip())[:400])
        return vr
    text = open(vb + ".metal", encoding="utf-8", errors="replace").read()
    fixed, _ = msl_fixups(text, passes)
    text = fixed if fixed is not None else text
    open(vb + ".metal", "w").write(text)
    vplan = msl_plan(text.replace(entry + "(", "main0(").replace(entry + "_out", "main0_out").replace(entry + "_in", "main0_in"))
    for k in ("sets", "discrete", "push", "other_buffers", "textures", "samplers"):
        if vplan.get(k) != plan.get(k):
            vr.update(status="plan_mismatch", error="binding plan differs from main0 in %s" % k)
            return vr
    vr["colors"] = vplan.get("features", {}).get("colors")
    rc, out, err, dt = run([tool("metal"), "-std=" + RECIPE["std"], *RECIPE["metal"], vb + ".metal", "-o", vb + ".air"],
                           RECIPE["metal_timeout"])
    vr["metal_s"] = round(dt, 3)
    if rc != 0:
        e = err.decode("utf-8", "replace")
        vr.update(status="metal_fail", error="TIMEOUT" if rc is None else
                  next((l.strip() for l in e.splitlines() if "error:" in l), e.strip())[:400])
        return vr
    vr["status"] = "ok"
    vr["air"] = vb + ".air"
    return vr


def _finish(r, meta_path, base, t_start):
    for v in (r.get("output_variants") or {}):
        for ext in (".spv", ".metal", ".air"):
            try:
                os.remove(base + "_o" + v + ext)
            except OSError:
                pass
    for ext in (".spv", ".metal", ".air"):
        try:
            os.remove(base + ext)
        except OSError:
            pass
    r["total_s"] = round(time.monotonic() - t_start, 3)
    tmpf = "%s.%d.%d.part" % (meta_path, os.getpid(), threading.get_ident())
    json.dump(r, open(tmpf, "w"))
    os.replace(tmpf, meta_path)
    return r


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("spv")
    ap.add_argument("--stage", required=True)
    ap.add_argument("--cas", required=True)
    ap.add_argument("--spirv-dir", default=None)
    ap.add_argument("--vs-route", default="lane", choices=["lane", "kernel"])
    ap.add_argument("--drop", default="")
    ap.add_argument("--keep-msl", action="store_true")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--dual-source", action="store_true",
                    help="ps: blend source 1 from Location 1 to Location 0 Index 1 (dual-source-blend PSOs)")
    ap.add_argument("--output-variants", default="",
                    help="ps: comma-separated output-type codes (f/u/i per colour location, tools/out_types.py); the "
                         "library gets an entry main0_o<code> for each that needs one")
    a = ap.parse_args()
    opts = {"vs_route": a.vs_route, "force": a.force, "dual_source": a.dual_source}
    if a.output_variants:
        import out_types
        opts["output_variants"] = [c for c in a.output_variants.split(",") if c]
        opts["output_signature"] = out_types.dxbc_targets(dxbc_of(a.spv))
    if a.drop:
        opts["drop_outputs"] = [int(x) for x in a.drop.split(",")]
    tr = translator_id(a.spirv_dir or os.path.dirname(a.spv))
    r = compile_one(a.spv, a.stage, a.cas, tr, opts, keep_msl=a.keep_msl)
    print(json.dumps(r, indent=1)[:4000])


if __name__ == "__main__":
    sys.exit(main())
