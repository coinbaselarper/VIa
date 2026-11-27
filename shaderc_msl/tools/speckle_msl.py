#!/usr/bin/env python3
"""Speckle / correctness rows: store recipe on a SPIR-V of the translator fork (or the corpus), the passes of the row
the title uses today (mspeed fp_inline / hash_memo when that row has them), then the MSL passes below; metallib and an
overlay row with the binding plan of the new MSL (tables and push columns recomputed).

  speckle_msl.py rows OUT NAME... [--spv DIR] [--names-file F] [--atomic64] [--early-depth] [--cs-lod] [--mesh-clamp]
                    [--safe-math] [--jobs N]
  speckle_msl.py census                     world shaders each fix touches (pcache tier names; FSSM, disassembly)

Passes:
  atomic64_texture       every write the fork marked (--image-atomic64) becomes a 64-bit texture atomic on the same
                         descriptor viewed as texture2d<ulong, access::read_write>; a marker left is an error
  early_fragment_tests   [[early_fragment_tests]] on every fragment entry (rows of shaders whose FSSM
                         DB_SHADER_CONTROL has DEPTH_BEFORE_SHADER and no Z export)
  safe_math              the same MSL compiled with -fno-fast-math (diagnostic: the store compiles with Metal's
                         default fast math)
"""
import argparse
import concurrent.futures as cf
import gzip
import hashlib
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.abspath(os.path.join(HERE, "..", ".."))
ROOT = os.path.dirname(PORT)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(PORT, "d3d12_metal", "tools"))
import mesh_tg_pack as P  # noqa: E402
import msl_instrument as MI  # noqa: E402
import mspeed  # noqa: E402
import mspeed_passes as MP  # noqa: E402
import shaderc_msl as R  # noqa: E402

VERSION = "speckle 1"
WORK = os.path.join(ROOT, "spike", "s2-make-game", "work", "shaders")
FSSM = os.path.join(WORK, "dxbc", "fssm")
TIER = os.path.join(PORT, "build", "pc7", "arch-world", "pcache-tier.txt")
DEFAULT_OVERLAYS = ["mspeed-x86_64/overlay.tsv", "expo-x86_64/rectvs/manifest.tsv", "gpass-x86_64/mop/overlay.tsv",
                    "tmem-x86_64/overlay.tsv", "mcc-x86_64/overlay.tsv", "d3d12_metal-msl-overrides/manifest.tsv",
                    "uintps-x86_64/overlay.tsv"]
MARK = {"1554098974u": "atomic_max", "1554098973u": "atomic_min"}
MARK2 = "1688666460u"
DB_DEPTH_BEFORE_SHADER = 1 << 12
DB_Z_EXPORT = 1


def db_shader_control(name):
    p = os.path.join(FSSM, name + ".fssm")
    if not os.path.exists(p):
        return None
    d = open(p, "rb").read()
    return struct.unpack_from("<I", d, 0x24C)[0] if len(d) >= 0x250 and d[:4] == b"FSSM" else None


def early_depth_wanted(name):
    v = db_shader_control(name)
    return v is not None and bool(v & DB_DEPTH_BEFORE_SHADER) and not v & DB_Z_EXPORT


def effective_rows(names):
    """The row the title loads for each name today: the first default overlay that lists it, else the store's."""
    head, store = P.manifest_rows()
    out = {}
    for rel in DEFAULT_OVERLAYS:
        for line in open(os.path.join(PORT, "build", rel)):
            if line.startswith("#"):
                continue
            f = line.rstrip("\n").split("\t")
            if f[0] in names and f[0] not in out:
                out[f[0]] = (rel, f)
    for n in names:
        if n not in out and n in store:
            out[n] = ("store", store[n])
    return out


def split_args(s):
    out, depth, cur = [], 0, []
    for c in s:
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        if c == "," and depth == 0:
            out.append("".join(cur).strip())
            cur = []
        else:
            cur.append(c)
    out.append("".join(cur).strip())
    return out


def receiver_start(s, dot):
    """Start of the `name[...]` expression that ends just before s[dot] ('.')."""
    i = dot - 1
    if s[i] != "]":
        raise ValueError("marked write on a receiver that is not a descriptor subscript")
    depth = 0
    while i >= 0:
        if s[i] == "]":
            depth += 1
        elif s[i] == "[":
            depth -= 1
            if depth == 0:
                break
        i -= 1
    j = i - 1
    while j >= 0 and (s[j].isalnum() or s[j] == "_"):
        j -= 1
    return j + 1


def atomic64_texture(msl):
    out, at, n = [], 0, 0
    for m in re.finditer(r"\.write\(uint4\(", msl):
        if m.start() < at:
            continue
        lp = m.end() - 1
        rp = mspeed.match_paren(msl, lp)
        args = split_args(msl[lp + 1:rp - 1])
        if len(args) != 4 or args[2] not in MARK or args[3] != MARK2:
            continue
        wlp = m.start() + len(".write")
        wrp = mspeed.match_paren(msl, wlp)
        wargs = split_args(msl[wlp + 1:wrp - 1])
        if len(wargs) != 2:
            raise ValueError("marked write with %d arguments" % len(wargs))
        rs = receiver_start(msl, m.start())
        recv = msl[rs:m.start()]
        call = ("(*static_cast<const device texture2d<ulong, access::read_write>*>(static_cast<const device void*>(&%s)))"
                ".%s(%s, (ulong(%s) << 32) | ulong(%s))" % (recv, MARK[args[2]], wargs[1], args[1], args[0]))
        out.append(msl[at:rs])
        out.append(call)
        at = wrp
        n += 1
    out.append(msl[at:])
    res = "".join(out)
    if MARK2 in res:
        raise ValueError("a 64-bit atomic marker is left after atomic64_texture")
    return res, n


def early_fragment_tests(msl):
    res, n = re.subn(r"(?m)^fragment ", "[[early_fragment_tests]] fragment ", msl)
    if not n:
        raise ValueError("no fragment entry")
    if re.search(r"\[\[depth\(", res):
        raise ValueError("a depth output cannot run with early fragment tests")
    return res, n


def regen(name, row, stage, spv_path, tmp):
    meta = MI.cas_meta(P.manifest_rows()[1][name], stage)
    if not meta:
        return None, None, "no CAS record"
    if any(p["pass"] in ("output_variants", "dual_source") for p in meta.get("passes", [])) or meta.get("output_variants"):
        return None, None, "row has output variants / dual source (not handled)"
    raw = open(spv_path, "rb").read()
    words = list(struct.unpack("<%dI" % (len(raw) // 4), raw))
    w, passes, extra = R.prepare(words, stage, MI.opts_from(meta))
    os.makedirs(tmp, exist_ok=True)
    base = os.path.join(tmp, hashlib.sha256((name + spv_path).encode()).hexdigest()[:16])
    open(base + ".spv", "wb").write(P.spv.to_bytes(w))
    rc, o, e, dt = R.run([R.SPIRV_CROSS, base + ".spv", *R.RECIPE["cross"], "--msl-version", str(R.RECIPE["msl_version"]),
                          *extra, "--output", base + ".metal"], R.RECIPE["cross_timeout"])
    if rc != 0:
        return None, None, "spirv-cross failed"
    text = open(base + ".metal", encoding="utf-8", errors="replace").read()
    for ext in (".spv", ".metal"):
        try:
            os.remove(base + ext)
        except OSError:
            pass
    fixed, _ = R.msl_fixups(text, passes)
    return (fixed if fixed is not None else text), passes, None


def build(out_dir, name, src, row, spv_path, want):
    r = {"name": name, "base": src}
    stage = row[2]
    tmp = os.path.join(out_dir, "tmp", str(os.getpid()))
    msl, rpasses, err = regen(name, row, stage, spv_path, tmp)
    if msl is None:
        r.update(status="regen_fail", error=err)
        return r
    names = [p["pass"] for p in rpasses]
    keep = [p for p in row[8].split(",") if p in MP.PASSES] if row[8] != "-" else []
    if keep:
        msl, info = MP.apply(msl, keep)
        names += [p for p in keep if p in info]
    counts = {}
    if "atomic64" in want:
        msl, counts["atomic64_texture"] = atomic64_texture(msl)
        if counts["atomic64_texture"]:
            names.append("atomic64_texture")
    if "cs_lod" in want:
        counts["cs_implicit_lod"] = msl.count("quad_broadcast(")
        if counts["cs_implicit_lod"]:
            names.append("cs_implicit_lod")
    if "mesh_clamp" in want:
        counts["mesh_index_clamp"] = len(re.findall(r"min\(_\w+, _\w+\)", msl)) and 1
        names.append("mesh_index_clamp")
    plan = R.msl_plan(msl)
    if "early_depth" in want:
        msl, counts["early_fragment_tests"] = early_fragment_tests(msl)
        names.append("early_fragment_tests")
    if "safe_math" in want:
        counts["safe_math"] = 1
        names.append("safe_math")
    if not any(counts.values()):
        r["status"] = "unchanged"
        return r
    key = hashlib.sha256((VERSION + "\n" + ",".join(names) + "\n" + msl).encode()).hexdigest()
    d = os.path.join(out_dir, "cas", key[:2])
    os.makedirs(d, exist_ok=True)
    lib = os.path.join(d, key + ".metallib")
    if not os.path.exists(lib):
        base = os.path.join(tmp, key[:16])
        open(base + ".metal", "w").write(msl)
        with gzip.open(os.path.join(d, key + ".metal.gz"), "wb", compresslevel=1) as g:
            g.write(msl.encode())
        rc, o, e, dt = R.run([R.tool("metal"), "-std=" + R.RECIPE["std"], *R.RECIPE["metal"],
                              *(["-fno-fast-math"] if "safe_math" in want else []), base + ".metal", "-o",
                              base + ".air"], R.RECIPE["metal_timeout"])
        if rc != 0:
            r.update(status="metal_fail", error=(e or b"").decode("utf-8", "replace")[-800:])
            return r
        part = "%s.%d.part" % (lib, os.getpid())
        rc, o, e, dt = R.run([R.tool("metallib"), base + ".air", "-o", part], R.RECIPE["metal_timeout"])
        if rc != 0:
            r["status"] = "metallib_fail"
            return r
        os.replace(part, lib)
        for ext in (".metal", ".air"):
            try:
                os.remove(base + ext)
            except OSError:
                pass
    r.update(status="ok", key=key, metallib=lib, passes=names, counts=counts,
             push=plan.get("push"), tables=",".join("%d:%d" % (int(s) - 1, i) for s, i in
                                                     sorted(plan["sets"].items(), key=lambda kv: int(kv[0]))))
    return r


def overlay_row(row, r):
    f = list(row)
    f[1] = r["key"]
    f[4] = os.path.relpath(r["metallib"], P.STORE_ROOT)
    f[5] = str(r["push"]) if r["push"] is not None else "-"
    f[6] = r["tables"] or "-"
    f[8] = ",".join(r["passes"])
    return f


def world_names():
    dec = lambda n: re.sub(r"~([0-9a-f]{2})", lambda m: chr(int(m.group(1), 16)), n)
    stage = {}
    for line in open(TIER):
        if line.startswith("#"):
            continue
        f = line.rstrip("\n").split("\t")
        if len(f) < 5:
            continue
        for m in re.finditer(r"\b(vs|ps|ms|cs|gs|as)=([^@ ]+)@", f[4]):
            stage[dec(m.group(2))] = m.group(1)
    return stage


def census():
    stage = world_names()
    dis = lambda n: os.path.join(WORK, "library", "disasm", n[8:] + ".s") if n.startswith("library/") else \
        os.path.join(WORK, "dxbc", "disasm", n + ".s")
    res = {"atomic64": [], "early_depth": [], "early_depth_uav": [], "early_depth_kill": [], "cs_implicit": [],
           "mesh": []}
    implicit_ops = 0
    for n, st in sorted(stage.items()):
        p = dis(n)
        if not os.path.exists(p):
            continue
        t = open(p).read()
        ops = re.findall(r"^\s*(image_\w+|buffer_\w+|global_\w+)([^;\n]*)", t, re.M)
        if any(o[0].startswith("image_atomic") and "dmask:0x3" in o[1] for o in ops):
            res["atomic64"].append(n)
        if st == "ps" and early_depth_wanted(n):
            res["early_depth"].append(n)
            if any(o[0].startswith(("image_store", "image_atomic", "buffer_store", "buffer_atomic", "global_store",
                                    "global_atomic")) for o in ops):
                res["early_depth_uav"].append(n)
            if db_shader_control(n) >> 6 & 1:
                res["early_depth_kill"].append(n)
        if st == "cs":
            k = [o for o in ops if re.match(r"image_sample(_c)?(_b)?(_o)?$", o[0])]
            if k:
                res["cs_implicit"].append(n)
                implicit_ops += len(k)
        if st == "ms":
            res["mesh"].append(n)
    print(json.dumps({k: len(v) for k, v in res.items()}), "cs implicit-LOD sample instructions", implicit_ops)
    return res


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    a1 = sub.add_parser("rows")
    a1.add_argument("out")
    a1.add_argument("names", nargs="*")
    a1.add_argument("--names-file")
    a1.add_argument("--spv", help="directory of the fork's SPIR-V (default: the corpus)")
    a1.add_argument("--atomic64", action="store_true")
    a1.add_argument("--early-depth", action="store_true")
    a1.add_argument("--cs-lod", action="store_true", help="the fork's --cs-implicit-lod SPIR-V (rows where it emitted quad gradients)")
    a1.add_argument("--mesh-clamp", action="store_true", help="the fork's --mesh-index-clamp SPIR-V")
    a1.add_argument("--safe-math", action="store_true", help="compile with -fno-fast-math")
    a1.add_argument("--jobs", type=int, default=2)
    a2 = sub.add_parser("census")
    a2.add_argument("--out")
    a = ap.parse_args()
    if a.cmd == "census":
        res = census()
        if a.out:
            json.dump(res, open(a.out, "w"), indent=0)
        return
    if a.names_file:
        a.names += [l.strip() for l in open(a.names_file) if l.strip()]
    want = set()
    if a.atomic64:
        want.add("atomic64")
    if a.early_depth:
        want.add("early_depth")
    if a.cs_lod:
        want.add("cs_lod")
    if a.mesh_clamp:
        want.add("mesh_clamp")
    if a.safe_math:
        want.add("safe_math")
    rows = effective_rows(set(a.names))
    os.makedirs(a.out, exist_ok=True)
    jobs = []
    with cf.ProcessPoolExecutor(a.jobs) as ex:
        for n in a.names:
            if n not in rows:
                print("no row:", n, file=sys.stderr)
                continue
            src, row = rows[n]
            spv = os.path.join(a.spv or P.SPIRV, n + ".spv")
            w = set(want)
            if "early_depth" in w and not early_depth_wanted(n):
                w.discard("early_depth")
            jobs.append((n, src, row, ex.submit(build, a.out, n, src, row, spv, w)))
        res = []
        for n, src, row, fu in jobs:
            try:
                r = fu.result()
            except Exception as e:  # noqa: BLE001
                r = {"name": n, "status": "error", "error": str(e)[:300]}
            r["row"] = row
            res.append(r)
    with open(os.path.join(a.out, "speckle.jsonl"), "w") as f:
        for r in res:
            f.write(json.dumps({k: v for k, v in r.items() if k != "row"}) + "\n")
    rel_root = os.path.relpath(P.STORE_ROOT, a.out)
    ok = [r for r in res if r.get("status") == "ok"]
    with open(os.path.join(a.out, "overlay.tsv"), "w") as f:
        f.write("# shaderc_msl manifest v3 root=%s\n" % rel_root)
        for r in ok:
            f.write("\t".join(overlay_row(r["row"], r)) + "\n")
    by = {}
    for r in res:
        by[r.get("status")] = by.get(r.get("status"), 0) + 1
    print(json.dumps(by), "->", os.path.join(a.out, "overlay.tsv"))
    for r in res:
        if r.get("status") != "ok":
            print(" ", r["name"], r.get("status"), (r.get("error") or "")[:200])


if __name__ == "__main__":
    main()
