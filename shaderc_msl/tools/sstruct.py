#!/usr/bin/env python3
"""Structural speed-up of translated shaders: re-translate with the translator fork
(port/shaders_vk/translator-fork, flags below), rebuild the store row's MSL with the mesh-speed passes, and A/B the
corpus translation against the fork's on the offline mesh probe (outputs bit-compared, time a group).

  sstruct.py spv OUTDIR NAME... [--flags=--fold-waterfalls,...]   fork translation of NAMEs into OUTDIR
  sstruct.py lib OUTDIR SPVDIR NAME...                            store recipe + private outputs + mspeed passes
  sstruct.py ab WORK NAME [--inputs zero,fol_a,...] [--groups G] [--flags="a,b;a,b,c"]
                                                                  A = corpus, B = fork: capture + safe variants,
                                                                  probe through tmem_slot.sh, compare, time
  sstruct.py abcs WORK library/NAME [--seeds 1,2] [--cs-groups 4,4,1] [--cs-threads 8,8,1] [--flags=...]
                                                                  compute: A = corpus, B = fork (cspeed passes), raw
                                                                  heap accesses bounded (safe_cs), sstruct_cs_ab
  sstruct.py manifest OUT.txt OVERLAY.tsv... MANIFEST...          pipelines of MANIFESTs that use an overlay's rows
  sstruct.py pert WORK NAME --edit "OLD=>NEW" [--trace-input fol_a] [--flags=...]
                                                                  exactness on an edited program (fork A vs fork B)
  sstruct.py abcap WORK library/NAME [--flags=...] [--reps N]     compute A/B on the title captures (cspeed_ab)
  sstruct.py div WORK NAME SPEC.json INPUTNAME                    input with lane-varying descriptor words: SPEC's
                                                                  heap, table filled with the keys a trace saw
  sstruct.py overlay OUT [NAME...] [--names-file F] [--flags=...] overlay rows (mspeed/cspeed layout) of the fork
"""
import argparse
import gzip
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.abspath(os.path.join(HERE, "..", ".."))
ROOT = os.path.dirname(PORT)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(PORT, "d3d12_metal", "tools"))
import mesh_tg_pack as P  # noqa: E402
import mspeed  # noqa: E402
import mspeed_cmp  # noqa: E402
import mspeed_overlay as MO  # noqa: E402

FORK = os.path.join(PORT, "shaders_vk", "translator-fork", "tools", "rdna2spirv.py")
DISASM = os.path.join(ROOT, "spike", "s2-make-game", "work", "shaders", "dxbc", "disasm")
CORPUS = P.SPIRV
BASE_FLAGS = ["--wave64-emulate", "--f16-interp-fallback"]
TREE = os.path.join(PORT, "build", "sstruct-x86_64")
PROBE = os.path.join(TREE, "probe", "gta6rt_sstruct_probe")
SLOT = os.path.join(HERE, "tmem_slot.sh")
INPUTS = os.path.join(PORT, "build", "mspeed-x86_64", "inputs")
MY_INPUTS = os.path.join(TREE, "inputs")
PY = "/opt/homebrew/bin/python3.14"


PARTS = {"": DISASM, "library": os.path.join(ROOT, "spike", "s2-make-game", "work", "shaders", "library", "disasm")}


def translate(out, names, flags):
    """Fork translation of store names ("NAME" or "library/NAME") into out (library ones into out/library)."""
    res = {}
    by_part = {}
    for n in names:
        part, _, base = n.rpartition("/")
        by_part.setdefault(part, []).append((n, base))
    for part, items in by_part.items():
        dst = os.path.join(out, part) if part else out
        os.makedirs(dst, exist_ok=True)
        stage = os.path.join(dst, ".stage", "disasm")
        shutil.rmtree(os.path.dirname(stage), ignore_errors=True)
        os.makedirs(stage)
        for n, base in items:
            os.symlink(os.path.join(PARTS[part], base + ".s"), os.path.join(stage, base + ".s"))
        r = subprocess.run([PY, "-I", FORK, stage, "--out", dst, "--no-asm", *BASE_FLAGS, *flags],
                           capture_output=True, text=True)
        shutil.rmtree(os.path.dirname(stage), ignore_errors=True)
        if r.returncode != 0:
            raise RuntimeError("translate failed: " + (r.stdout + r.stderr)[-1500:])
        idx = json.load(open(os.path.join(dst, "index.json")))
        for n, base in items:
            res[n] = idx.get(base)
    return res


CS_PASSES = ["merge_typed_reads", "simd_votes", "fp_inline", "hash_memo"]


def build_cs(out_dir, spv_root, name, row, passes):
    """cspeed_overlay.build with the SPIR-V read from spv_root (the store's recipe, no MSL size check: the fork's
    module is not the one the store record was made from)."""
    import msl_instrument as MI
    import mspeed_passes as MP
    import shaderc_msl as R
    r = {"name": name}
    tmp = os.path.join(out_dir, "tmp", str(os.getpid()))
    os.makedirs(tmp, exist_ok=True)
    meta = MI.cas_meta(row, "cs")
    if not meta:
        r["status"] = "no_cas_record"
        return r
    raw = open(os.path.join(spv_root, name + ".spv"), "rb").read()
    words = list(P.spv.struct.unpack("<%dI" % (len(raw) // 4), raw))
    w, rpasses, extra = R.prepare(words, "cs", MI.opts_from(meta))
    base = os.path.join(tmp, hashlib.sha256((name + spv_root).encode()).hexdigest()[:16])
    open(base + ".spv", "wb").write(P.spv.to_bytes(w))
    rc, o, e, dt = R.run([R.SPIRV_CROSS, base + ".spv", *R.RECIPE["cross"], "--msl-version", str(R.RECIPE["msl_version"]),
                          *extra, "--output", base + ".metal"], R.RECIPE["cross_timeout"])
    if rc != 0:
        r["status"] = "cross_fail"
        return r
    text = open(base + ".metal", encoding="utf-8", errors="replace").read()
    fixed, _ = R.msl_fixups(text, rpasses)
    msl = fixed if fixed is not None else text
    names = [p["pass"] for p in meta["passes"]]
    out, info, applied = msl, {}, []
    for p in passes:
        try:
            new, i = MP.PASSES[p](out)
        except (ValueError, AttributeError, KeyError, IndexError) as ex:
            info[p] = {"why": "not applicable: %s" % str(ex)[:120]}
            continue
        info[p] = i
        if new != out:
            out = new
            applied.append(p)
    names += applied
    key = hashlib.sha256(("sstruct cs 1\n" + ",".join(applied) + "\n" + out).encode()).hexdigest()
    d = os.path.join(out_dir, "cas", key[:2])
    os.makedirs(d, exist_ok=True)
    lib = os.path.join(d, key + ".metallib")
    if not os.path.exists(lib):
        with gzip.open(os.path.join(d, key + ".metal.gz"), "wb", compresslevel=1) as g:
            g.write(out.encode())
        dt = compile_msl(out, base + ".metal")
        os.replace(base + ".metallib", lib)
        r["metal_s"] = round(dt, 1)
    for ext in (".spv", ".metal"):
        try:
            os.remove(base + ext)
        except OSError:
            pass
    r.update(status="ok", key=key, metallib=lib, passes=names, info=info)
    return r


SAFE_CS_HELPER = r"""
static inline ulong sstruct_clamp(ulong a, const device uint* t)
{
    ulong base = (ulong(t[1]) << 32) | ulong(t[0]);
    ulong bytes = ulong(t[2]) * 4ul;
    return (a >= base && a - base < bytes - 256ul) ? a : base + bytes - 256ul;
}
"""
SAFE_CAST = re.compile(r"reinterpret_cast<(?:const )?device (?:spvUnsafeArray<(?:uint|int|float), \d+>|uint|int|float|atomic_uint|atomic_int)\*>\(")


def safe_cs(msl):
    """Every raw heap access (load, store, atomic) whose address is outside buffer 29's heap range goes to a sink at the
    heap's end (A and B get the same rewrite: in-range accesses are unchanged)."""
    out, i, n = [], 0, 0
    for m in SAFE_CAST.finditer(msl):
        if m.start() < i:
            continue
        lp = m.end() - 1
        rp = mspeed.match_paren(msl, lp)
        out.append(msl[i:lp + 1])
        out.append("sstruct_clamp(" + msl[lp + 1:rp - 1] + ", _mtr)")
        i = rp - 1
        n += 1
    out.append(msl[i:])
    msl = "".join(out)
    msl = msl.replace("using namespace metal;\n", "using namespace metal;\n" + SAFE_CS_HELPER, 1)
    f = mspeed.functions(msl)
    users = [nm for nm, (s0, lp, rp, bs, be) in f.items() if nm != "main0" and "sstruct_clamp(" in msl[bs:be]]
    need = mspeed.callers_closure(msl, set(users))
    msl = mspeed.add_param(msl, sorted(need), "const device uint* _mtr", "_mtr")
    s0, lp, rp, bs, be = mspeed.functions(msl)["main0"]
    msl = msl[:rp - 1] + ", const device uint* _mtr [[buffer(29)]]" + msl[rp - 1:]
    return msl, n


CS_AB = os.path.join(TREE, "probe", "gta6rt_sstruct_cs_ab")


def ab_cs(work, name, flags, seeds, groups, threads, reps, layout="safe"):
    os.makedirs(work, exist_ok=True)
    tag = hashlib.sha256(name.encode()).hexdigest()[:10]
    fork_dir = os.path.join(work, "spv-" + tag)
    meta = translate(fork_dir, [name], flags)[name]
    head, rows = P.manifest_rows()
    row = rows[name]
    res = {"name": name, "flags": flags, "fork_meta": {k: (meta or {}).get(k) for k in ("control_flow", "waterfalls_folded")}}
    ra = build_cs(work, CORPUS, name, row, CS_PASSES)
    rb = build_cs(work, fork_dir, name, row, CS_PASSES)
    for k, r in (("a", ra), ("b", rb)):
        if r.get("status") != "ok":
            raise RuntimeError("%s build %s" % (k, r.get("status")))
    libs = {}
    for k, r in (("a", ra), ("b", rb)):
        msl = msl_of(r)
        res["cases_" + k] = len(re.findall(r"\n\s*case \d+u:", msl))
        sm, n = safe_cs(msl)
        path = os.path.join(work, "%s.%s.safe.metal" % (tag, k))
        res["metal_s_" + k] = round(compile_msl(sm, path), 1)
        libs[k] = (path[: -len(".metal")] + ".metallib", path)
    res["seeds"] = {}
    for seed in seeds:
        cfg = os.path.join(work, "%s.seed%d.json" % (tag, seed))
        r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "tmem_abcfg.py"), libs["a"][1], cfg, "--seed", str(seed),
                            "--groups", groups, "--threads", threads, "--layout", layout, "--heap-mib", "32"],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("abcfg: " + r.stderr[-400:])
        r = subprocess.run([SLOT, CS_AB, libs["a"][0], libs["b"][0], cfg, "--time", str(reps)], capture_output=True, text=True)
        line = {"rc": r.returncode}
        for l in r.stdout.splitlines():
            if l.startswith(("RESULT", "SINK")):
                line.setdefault("result", []).append(l)
            elif l.startswith("DIFF"):
                line.setdefault("diff", []).append(l)
            elif l.startswith("TIME"):
                line.setdefault("time", []).append(l)
            elif "error" in l:
                line["error"] = l
        res["seeds"][seed] = line
        if r.returncode != 0:
            break
    res["lib_a"], res["lib_b"] = ra["metallib"], rb["metallib"]
    return res


def build_lib(out, spv_dir, name, passes=None):
    head, rows = P.manifest_rows()
    P.SPIRV = spv_dir
    try:
        return MO.build(out, name, rows[name], passes or MO.DEFAULT_PASSES)
    finally:
        P.SPIRV = CORPUS


def msl_of(r):
    lib = r["metallib"]
    with gzip.open(lib[: -len(".metallib")] + ".metal.gz", "rt") as f:
        return f.read()


def compile_msl(msl, path):
    open(path, "w").write(msl)
    return mspeed.build_lib(path, path[: -len(".metal")] + ".metallib")


ARGKIND = {"texture2d": "tex2d", "texture1d": "tex1d", "texture3d": "tex3d", "texture2d_array": "tex2darr",
           "texturecube": "texcube", "sampler": "sampler"}


def mesh_job(msl, libs, inp, groups, reps, prefix, rec_words):
    args = {}
    for m in re.finditer(r"struct spvDescriptorSetBuffer(\d+)\n\{\n(.*?)\n\};", msl, re.S):
        first = m.group(2).split("\n")[0]
        t = re.search(r"spvDescriptor<(\w+)<", first) or re.search(r"spvDescriptor<(sampler)>", first)
        if t:
            args[m.group(1)] = ARGKIND.get(t.group(1), "tex2d")
        elif "const device" in first:
            args[m.group(1)] = "table"
    sig = re.search(r"\[\[mesh\]\] void main0\((.*?)\)\n", msl).group(1)
    push = re.search(r"constant \w+& \w+ \[\[buffer\((\d+)\)\]\]", sig)
    st = re.search(r"struct spvPerVertex\n\{\n(.*?)\n\};", msl, re.S).group(1)
    locs = [int(l) for _, l in re.findall(r"float4 m_(\d+) \[\[user\(locn(\d+)\)\]\];", st)]
    j = {"libs": libs, "entry": "main0", "push_index": int(push.group(1)) if push else 8, "heap_bytes": 67108864,
         "argbufs": args, "textures": 16, "locations": locs, "groups": groups, "reps": reps, "trace_index": 29,
         "out_index": 30, "out_bytes": groups * 129 * rec_words * 4, "out_prefix": prefix}
    if inp == "zero":
        j["push"] = [0] * 36
    else:
        d = MY_INPUTS if os.path.exists(os.path.join(MY_INPUTS, inp + ".push.json")) else INPUTS
        j["push"] = json.load(open(os.path.join(d, inp + ".push.json")))
        j["heap_image"] = os.path.join(d, inp + ".heap")
        j["hash_words"] = os.path.join(d, inp + ".hash")
    return j


def div_input(work, name, spec_path, out_name, groups=256):
    """An input whose descriptor words differ by lane: the heap of SPEC, the bindless table filled with the keys a
    trace variant of the corpus translation looked up on it (each key a different texture)."""
    import mspeed_inputs as MI
    import mspeed_trace as MT
    os.makedirs(work, exist_ok=True)
    os.makedirs(MY_INPUTS, exist_ok=True)
    spec = json.load(open(spec_path))
    base = os.path.join(MY_INPUTS, out_name)
    tmp_spec = base + ".spec.json"
    json.dump(spec, open(tmp_spec, "w"))
    r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "mspeed_inputs.py"), tmp_spec, base], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(r.stderr[-500:])
    ra = build_lib(work, CORPUS, name)
    msl = msl_of(ra)
    tr, n = mspeed.trace(msl)
    path = os.path.join(work, "trace.metal")
    compile_msl(tr, path)
    lib = path[: -len(".metal")] + ".metallib"
    prefix = os.path.join(work, "trace")
    st = re.search(r"struct spvPerVertex\n\{\n(.*?)\n\};", msl, re.S).group(1)
    job = mesh_job(msl, [lib], out_name, groups, 1, prefix, 8 + 4 * len(re.findall(r"float4 m_(\d+) \[\[user", st)))
    job.pop("out_index")
    job["out_bytes"] = 0
    jp = prefix + ".job.json"
    json.dump(job, open(jp, "w"))
    probe(jp)
    tp = "%s.%s.trace" % (prefix, os.path.basename(lib))
    keys = [k for k in MT.summary(MT.load(tp))["keys"] if any(k)]
    os.remove(tp)
    t, mapped = MI.hash_table(keys, 16)
    import struct as _st
    open(base + ".hash", "wb").write(_st.pack("<%dI" % len(t), *t))
    return {"keys": len(keys), "placed": len(mapped)}


def probe(job_path):
    r = subprocess.run([SLOT, PROBE, job_path], capture_output=True, text=True)
    times = {}
    for line in r.stdout.splitlines():
        f = line.split("\t")
        if f[0] == "TIME":
            m = re.search(r"([\d.]+) us a group \(best\)\t([\d.]+) \(median\)", line)
            times[f[1]] = (float(m.group(1)), float(m.group(2)))
        elif f[0] == "FAIL":
            raise RuntimeError("probe: " + line)
    if r.returncode != 0:
        raise RuntimeError("probe rc %d: %s" % (r.returncode, (r.stdout + r.stderr)[-800:]))
    return times


def ab(work, name, inputs, groups, reps, variants):
    """A = the corpus translation, B1..Bn = the fork with each flag set; one probe job per input times all of them
    on the same inputs; every B is bit-compared with A."""
    os.makedirs(work, exist_ok=True)
    tag = hashlib.sha256(name.encode()).hexdigest()[:10]
    res = {"name": name, "variants": [",".join(v) for v in variants]}
    builds = [("a", CORPUS, None)]
    for k, flags in enumerate(variants):
        fork_dir = os.path.join(work, "spv-%s-%d" % (tag, k))
        meta = translate(fork_dir, [name], flags)[name] or {}
        res["fork_meta_b%d" % k] = {x: meta.get(x) for x in ("control_flow", "waterfalls_folded")}
        builds.append(("b%d" % k, fork_dir, flags))
    out = {}
    for k, spv_dir, flags in builds:
        r = build_lib(work, spv_dir, name)
        if r.get("status") != "ok":
            raise RuntimeError("%s build %s: %s" % (k, r.get("status"), r.get("error", "")))
        res["lib_" + k] = r["metallib"]
        msl = msl_of(r)
        res["cases_" + k] = len(re.findall(r"\n\s*case \d+u:", msl))
        safe, _ = mspeed.safe(msl)
        cap, info = mspeed.capture(safe)
        path = os.path.join(work, "%s.%s.cap.metal" % (tag, k))
        res["metal_s_" + k] = round(compile_msl(cap, path), 1)
        out[k] = (path[: -len(".metal")] + ".metallib", info, msl)
    rec = out["a"][1]["record_words"]
    if any(v[1]["record_words"] != rec for v in out.values()):
        raise RuntimeError("output layout differs")
    keys = [b[0] for b in builds]
    res["inputs"] = {}
    for inp in inputs:
        prefix = os.path.join(work, "%s.%s" % (tag, inp))
        job = mesh_job(out["a"][2], [out[k][0] for k in keys], inp, groups, reps, prefix, rec)
        jp = prefix + ".job.json"
        json.dump(job, open(jp, "w"))
        t = probe(jp)
        fa = "%s.%s.out" % (prefix, os.path.basename(out["a"][0]))
        ri = {"us_group_a": t[os.path.basename(out["a"][0])]}
        for k in keys[1:]:
            fb = "%s.%s.out" % (prefix, os.path.basename(out[k][0]))
            st, first = mspeed_cmp.compare(rec, fa, fb)
            ri["us_group_" + k] = t[os.path.basename(out[k][0])]
            ri["cmp_" + k] = st
            ri["first_" + k] = first[:3]
            os.remove(fb)
        os.remove(fa)
        for f in os.listdir(work):
            if f.endswith(".trace"):
                os.remove(os.path.join(work, f))
        res["inputs"][inp] = ri
    return res


def perturbed_disasm(work, name, edits):
    """A copy of NAME's disassembly with operand text edits (same instruction, same encoding bytes, so every pc stays),
    in a folder laid out like the corpus' (container, fssm sidecar and index linked), for exactness tests."""
    src_root = os.path.dirname(DISASM)
    root = os.path.join(work, "pert")
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(os.path.join(root, "disasm"))
    os.makedirs(os.path.join(root, "fssm"))
    for rel in (name + ".dxbc", "index.json", os.path.join("fssm", name + ".fssm")):
        if os.path.exists(os.path.join(src_root, rel)):
            os.symlink(os.path.join(src_root, rel), os.path.join(root, rel))
    text = open(os.path.join(DISASM, name + ".s")).read()
    for old, new_ in edits:
        if text.count(old) != 1:
            raise RuntimeError("edit %r occurs %d times" % (old, text.count(old)))
        text = text.replace(old, new_)
    open(os.path.join(root, "disasm", name + ".s"), "w").write(text)
    return os.path.join(root, "disasm")


def ab_pert(work, name, edits, inputs, groups, reps, flags, trace_spec=None):
    """Exactness test on a perturbed program: A = fork without flags, B = fork with flags, both from the same edited
    disassembly; the bindless table is filled with the keys a trace of A looks up on the trace input."""
    global DISASM
    d = perturbed_disasm(work, name, edits)
    saved = DISASM
    DISASM = d
    try:
        tag = hashlib.sha256(name.encode()).hexdigest()[:10]
        da, db = os.path.join(work, "pa-" + tag), os.path.join(work, "pb-" + tag)
        translate(da, [name], [])
        meta = translate(db, [name], flags)[name] or {}
    finally:
        DISASM = saved
    res = {"name": name, "edits": edits, "flags": flags, "waterfalls_folded": meta.get("waterfalls_folded")}
    ra, rb = build_lib(work, da, name), build_lib(work, db, name)
    out = {}
    for k, r in (("a", ra), ("b", rb)):
        if r.get("status") != "ok":
            raise RuntimeError("%s build %s" % (k, r.get("status")))
        msl = msl_of(r)
        safe, _ = mspeed.safe(msl)
        cap, info = mspeed.capture(safe)
        path = os.path.join(work, "%s.%s.pcap.metal" % (tag, k))
        compile_msl(cap, path)
        out[k] = (path[: -len(".metal")] + ".metallib", info, msl)
    rec = out["a"][1]["record_words"]
    if trace_spec:
        import mspeed_inputs as MI
        import mspeed_trace as MT
        tr, _ = mspeed.trace(out["a"][2])
        tp = os.path.join(work, "%s.ptrace.metal" % tag)
        compile_msl(tr, tp)
        lib = tp[: -len(".metal")] + ".metallib"
        prefix = os.path.join(work, "%s.ptrace" % tag)
        job = mesh_job(out["a"][2], [lib], trace_spec, 256, 1, prefix, rec)
        job.pop("out_index")
        job["out_bytes"] = 0
        json.dump(job, open(prefix + ".job.json", "w"))
        probe(prefix + ".job.json")
        tf = "%s.%s.trace" % (prefix, os.path.basename(lib))
        keys = [k for k in MT.summary(MT.load(tf))["keys"] if any(k)]
        os.remove(tf)
        t, mapped = MI.hash_table(keys, 16)
        os.makedirs(MY_INPUTS, exist_ok=True)
        name_in = "pert_" + tag
        import struct as _st
        for ext in (".heap", ".push.json"):
            src = os.path.join(MY_INPUTS if os.path.exists(os.path.join(MY_INPUTS, trace_spec + ext)) else INPUTS, trace_spec + ext)
            shutil.copyfile(src, os.path.join(MY_INPUTS, name_in + ext))
        open(os.path.join(MY_INPUTS, name_in + ".hash"), "wb").write(_st.pack("<%dI" % len(t), *t))
        res["trace_keys"] = len(keys)
        inputs = list(inputs) + [name_in]
    res["inputs"] = {}
    for inp in inputs:
        prefix = os.path.join(work, "%s.p.%s" % (tag, inp))
        job = mesh_job(out["a"][2], [out["a"][0], out["b"][0]], inp, groups, reps, prefix, rec)
        json.dump(job, open(prefix + ".job.json", "w"))
        t = probe(prefix + ".job.json")
        fa = "%s.%s.out" % (prefix, os.path.basename(out["a"][0]))
        fb = "%s.%s.out" % (prefix, os.path.basename(out["b"][0]))
        st, first = mspeed_cmp.compare(rec, fa, fb)
        res["inputs"][inp] = {"cmp": st, "first": first[:3], "us_group": [t[os.path.basename(out[k][0])] for k in ("a", "b")]}
        for f in (fa, fb):
            os.remove(f)
    for f in os.listdir(work):
        if f.endswith(".trace"):
            os.remove(os.path.join(work, f))
    return res


def overlay(out_dir, names, flags):
    """Overlay rows (store layout, paths relative to the store root) for NAMEs re-translated with the fork flags:
    mesh rows as mspeed_overlay builds them (store recipe, private outputs or split kept, the mesh-speed passes), library
    compute rows as cspeed_overlay does (store recipe + CS_PASSES). A name the flags leave unchanged gets no row."""
    out_dir = os.path.abspath(out_dir)
    os.makedirs(out_dir, exist_ok=True)
    head, rows = P.manifest_rows()
    spv = os.path.join(out_dir, "spv")
    metas = translate(spv, names, flags)
    results = {}
    for n in names:
        row = rows.get(n)
        r = {"name": n, "flags": flags, "fork": {k: (metas.get(n) or {}).get(k) for k in ("control_flow", "waterfalls_folded")}}
        part, _, base = n.rpartition("/")
        fork_spv = os.path.join(spv, part, base + ".spv") if part else os.path.join(spv, base + ".spv")
        corpus_spv = os.path.join(CORPUS, n + ".spv")
        if row is None:
            r["status"] = "not_in_store"
        elif not os.path.exists(fork_spv):
            r["status"] = "not_translated"
        elif open(fork_spv, "rb").read() == open(corpus_spv, "rb").read():
            r["status"] = "same_spirv"
        elif row[2] == "ms":
            r.update(build_lib(out_dir, spv, n))
        elif row[2] == "cs":
            r.update(build_cs(out_dir, spv, n, row, CS_PASSES))
        else:
            r["status"] = "stage " + row[2]
        results[n] = r
        print(json.dumps({k: v for k, v in r.items() if k not in ("info", "metallib")}), flush=True)
    log = os.path.join(out_dir, "sstruct.jsonl")
    old = {}
    if os.path.exists(log):
        for line in open(log):
            x = json.loads(line)
            old[x["name"]] = x
    old.update(results)
    with open(log + ".part", "w") as f:
        for r in old.values():
            f.write(json.dumps({k: v for k, v in r.items() if k != "info"}) + "\n")
    os.replace(log + ".part", log)
    rel_root = os.path.relpath(P.STORE_ROOT, out_dir)
    with open(os.path.join(out_dir, "overlay.tsv.part"), "w") as f:
        for h in head:
            f.write(re.sub(r"root=\S+", "root=" + rel_root, h) if "root=" in h else h)
        for n in sorted(old):
            r = old[n]
            if r.get("status") != "ok" or n not in rows:
                continue
            row = rows[n]
            if row[2] == "ms":
                fr = MO.overlay_row(row, r)
            else:
                fr = list(row)
                fr[1] = r["key"]
                fr[4] = os.path.relpath(r["metallib"], P.STORE_ROOT)
                fr[8] = ",".join(r["passes"])
            f.write("\t".join(fr) + "\n")
    os.replace(os.path.join(out_dir, "overlay.tsv.part"), os.path.join(out_dir, "overlay.tsv"))
    return results


def prebuild_manifest(out, overlays, sources):
    """The pipelines (pcache manifest lines of SOURCES) that use a shader one of OVERLAYS has a row for: what
    prepare_shaders must build for those rows."""
    names = set()
    for o in overlays:
        for line in open(o):
            if line.strip() and not line.startswith("#"):
                names.add(line.split("\t", 1)[0])
    dec = lambda t: re.sub(r"~([0-9a-f]{2})", lambda m: chr(int(m.group(1), 16)), t)  # noqa: E731
    head, seen, lines = [], set(), []
    for src in sources:
        for line in open(src):
            if line.startswith("#"):
                if not head or line not in head:
                    head.append(line) if len(head) < 2 else None
                continue
            f = line.rstrip("\n").split("\t")
            if len(f) < 5 or f[0] in seen:
                continue
            used = {dec(x.split("=", 1)[1].rsplit("@", 1)[0]) for x in f[4].split() if re.match(r"(ms|ps|vs|cs|os)=", x)}
            if used & names:
                seen.add(f[0])
                lines.append(line if line.endswith("\n") else line + "\n")
    with open(out, "w") as fo:
        fo.writelines(head[:2] + lines)
    return len(lines), len(names)


CSPEED_AB = os.path.join(TREE, "probe", "gta6rt_sstruct_cspeed_ab")
CAPS = os.path.join(PORT, "build", "cspeed-x86_64", "cap2")


def ab_cs_cap(work, name, flags, reps, shared_writes=True):
    """Compute A/B on the compute-speed worker's title captures (GTA6RT_METAL_CS_CAPTURE files, cspeed_capcfg.py,
    cspeed_ab.mm): A = corpus + CS_PASSES, B = fork + CS_PASSES, every writable texture and the heap compared."""
    os.makedirs(work, exist_ok=True)
    tag = hashlib.sha256(name.encode()).hexdigest()[:10]
    fork_dir = os.path.join(work, "spv-" + tag)
    translate(fork_dir, [name], flags)
    head, rows = P.manifest_rows()
    row = rows[name]
    ra = build_cs(work, CORPUS, name, row, CS_PASSES)
    rb = build_cs(work, fork_dir, name, row, CS_PASSES)
    res = {"name": name, "flags": flags, "lib_a": ra["metallib"], "lib_b": rb["metallib"], "caps": {}}
    msl_a = os.path.join(work, tag + ".a.metal")
    open(msl_a, "w").write(msl_of(ra))
    caps = sorted(f for f in os.listdir(CAPS) if f.startswith(name.replace("/", "_") + "_") and f.endswith(".cap"))
    for c in caps:
        prefix = os.path.join(work, "%s.%s" % (tag, c[:-4]))
        r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "cspeed_capcfg.py"), msl_a, os.path.join(CAPS, c), prefix],
                           capture_output=True, text=True)
        if r.returncode != 0:
            res["caps"][c] = {"error": "capcfg: " + (r.stdout + r.stderr)[-300:]}
            continue
        cmd = [SLOT, CSPEED_AB, ra["metallib"], rb["metallib"], prefix + ".json", "--time", str(reps)]
        if shared_writes:
            cmd.append("--shared-writes")
        r = subprocess.run(cmd, capture_output=True, text=True)
        out = {"rc": r.returncode, "lines": [l for l in r.stdout.splitlines() if l.startswith(("RESULT", "TIME", "DETERMINISM", "RUN"))
                                              or "error" in l.lower()][:12]}
        res["caps"][c] = out
        for ext in (".heap", ".table"):
            try:
                os.remove(prefix + ext)
            except OSError:
                pass
        if r.returncode != 0:
            break
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd")
    ap.add_argument("rest", nargs="*")
    ap.add_argument("--flags", default="--fold-waterfalls")
    ap.add_argument("--inputs", default="zero,fol_a,fol_s")
    ap.add_argument("--groups", type=int, default=2048)
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--seeds", default="1,2")
    ap.add_argument("--names-file", default=None)
    ap.add_argument("--edit", action="append", default=[], help="pert: OLD=>NEW operand text edit of the .s")
    ap.add_argument("--trace-input", default=None, help="pert: fill the table with the keys A looks up on this input")
    ap.add_argument("--cs-groups", default="4,4,1")
    ap.add_argument("--cs-threads", default="8,8,1")
    ap.add_argument("--cs-layout", default="safe", choices=["safe", "small"])
    a = ap.parse_args()
    flags = [f for f in a.flags.split(",") if f]
    if a.cmd == "spv":
        print(json.dumps(translate(a.rest[0], a.rest[1:], flags), indent=1)[:4000])
    elif a.cmd == "lib":
        for n in a.rest[2:]:
            print(json.dumps(build_lib(a.rest[0], a.rest[1], n)))
    elif a.cmd == "abcap":
        print(json.dumps(ab_cs_cap(a.rest[0], a.rest[1], flags, a.reps)))
    elif a.cmd == "abcs":
        r = ab_cs(a.rest[0], a.rest[1], flags, [int(x) for x in a.seeds.split(",")], a.cs_groups, a.cs_threads, a.reps,
                  a.cs_layout)
        print(json.dumps(r))
    elif a.cmd == "overlay":
        names = list(a.rest[1:])
        if a.names_file:
            names += [l.strip() for l in open(a.names_file) if l.strip()]
        overlay(a.rest[0], names, flags)
    elif a.cmd == "manifest":
        out, rest = a.rest[0], a.rest[1:]
        ovs = [x for x in rest if x.endswith(".tsv")]
        srcs = [x for x in rest if not x.endswith(".tsv")]
        print("pipelines %d for %d rows" % prebuild_manifest(out, ovs, srcs))
    elif a.cmd == "pert":
        edits = [tuple(e.split("=>", 1)) for e in a.edit]
        r = ab_pert(a.rest[0], a.rest[1], edits, a.inputs.split(","), a.groups, a.reps, flags, a.trace_input)
        print(json.dumps(r))
    elif a.cmd == "div":
        print(json.dumps(div_input(a.rest[0], a.rest[1], a.rest[2], a.rest[3])))
    elif a.cmd == "ab":
        variants = [[f for f in v.split(",") if f] for v in a.flags.split(";")]
        r = ab(a.rest[0], a.rest[1], a.inputs.split(","), a.groups, a.reps, variants)
        print(json.dumps(r))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
