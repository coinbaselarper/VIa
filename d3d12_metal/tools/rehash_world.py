#!/usr/bin/env python3
"""The Metal store's descriptor-table lookups from bare FNV-1a to fnv1a_fmix32 (portable/src/descriptor_table.h), for
every library a world run loads, as one overlay placed first.

  rehash_world.py plan WORK [--limit N] [--sample N]   resolve the tier's shaders through the default overlay list
  rehash_world.py build WORK [--jobs J]                one job per library (resumable: a finished job is skipped)
  rehash_world.py overlay WORK                         WORK/overlay.tsv (rows whose libraries changed) + WORK/report.json
  rehash_world.py manifest WORK                        WORK/prebuild.manifest.txt + WORK/catalog.tsv for prepare_shaders

A library's MSL comes from its sibling <key>.metal.gz / <key>.msl (the overlays keep the text they compiled), else it
is rebuilt from the corpus SPIR-V with shaderc_msl.compile_one and the options its result row records (the row's own
key is recomputed first: a mismatch fails the job). shaderc_msl.hash_fmix32 then wraps every 8-word FNV expression; a
library with no lookup keeps its metallib. Sources this cannot read fail the job loudly (report.json lists them).
Runs need: GTA6RT_METAL_TABLE_HASH=fnv1a_fmix32, GTA6RT_METAL_SHADER_OVERLAY=WORK/overlay.tsv:<default list>,
GTA6RT_METAL_ARCHIVE_TIERS=<port>/build/pc7/arch-world:WORK/arch, SHADERC_HASH_KIND=fnv1a_fmix32 (on-demand builds).
"""
import argparse
import gzip
import hashlib
import json
import os
import re
import shutil
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed

PORT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ROOT = os.path.dirname(PORT)
sys.path.insert(0, os.path.join(PORT, "shaderc_msl", "tools"))
import shaderc_msl as R  # noqa: E402

TIER = os.path.join(PORT, "build", "pc7", "arch-world", "pcache-tier.txt")
MAIN = os.path.join(PORT, "build", "shaderc_msl", "runs", "main", "manifest.tsv")
SPIRV = os.path.join(ROOT, "spike", "s2-make-game", "work", "shaders", "spirv")
DEFAULT_OVERLAYS = ["build/mspeed-x86_64/overlay.tsv", "build/expo-x86_64/rectvs/manifest.tsv",
                    "build/gpass-x86_64/mop/overlay.tsv", "build/tmem-x86_64/overlay.tsv", "build/mcc-x86_64/overlay.tsv",
                    "build/d3d12_metal-msl-overrides/manifest.tsv", "build/uintps-x86_64/overlay.tsv"]
LIB_COLS = (4, 11, 12, 18, 19)
KNOWN_PASSES = {"split_bindings", "x_buf_device", "dual_source", "output_variants", "lower_vertex_lane",
                "vertex_for_tessellation", "drop_outputs", "shrink_lds", "noinline_hot_helpers", "merge_typed_reads",
                "loop_cap", "soft_f64", "argbuf_raster_order_strip", "cull_distance_local", "trim_outputs",
                "pack_mesh_struct"}


FNV_WORD = 0x811C9DC5


def spv_of(meta, names):
    for n in names:
        p = os.path.join(SPIRV, n + ".spv")
        if os.path.exists(p):
            return p
    return os.path.join(SPIRV, meta["spv"])


def lookup_count(lib, names):
    """Cheap: FNV basis sites in the MSL, or 1/0 for its presence in the corpus SPIR-V."""
    stem, kind = lib[:-9], source_kind(lib)
    if kind == "msl":
        return open(stem + ".msl").read().count(R.FNV_BASIS)
    if kind == "metal.gz":
        return gzip.open(stem + ".metal.gz", "rt").read().count(R.FNV_BASIS)
    if kind == "spirv":
        meta = json.load(open(stem + ".json"))
        raw = open(spv_of(meta, names), "rb").read()
        return int(FNV_WORD in R.spv.struct.unpack("<%dI" % (len(raw) // 4), raw))
    return -1


def count(work):
    p = json.load(open(os.path.join(work, "plan.json")))
    by = {}
    for j in p["jobs"]:
        k = source_kind(j["lib"])
        n = lookup_count(j["lib"], j["names"])
        e = by.setdefault(k, {"libraries": 0, "with_lookups": 0})
        e["libraries"] += 1
        e["with_lookups"] += int(n > 0)
    json.dump(by, open(os.path.join(work, "count.json"), "w"), indent=1)
    print(json.dumps(by))


def dec(t):
    return re.sub(r"~([0-9a-f]{2})", lambda m: chr(int(m.group(1), 16)), t)


def manifest_root(path):
    with open(path) as f:
        head = f.readline()
    m = re.search(r"root=(\S+)", head)
    return os.path.normpath(os.path.join(os.path.dirname(path), m.group(1) if m else "."))


def tier_names():
    names = set()
    for line in open(TIER):
        if line.startswith("#"):
            continue
        for m in re.finditer(r"\b(?:vs|ps|ms|cs|os)=([^ @]+)@", line):
            names.add(dec(m.group(1)))
    return names


def plan(work, limit, sample):
    os.makedirs(work, exist_ok=True)
    names = tier_names()
    won, jobs = {}, {}
    for ov in [os.path.join(PORT, o) for o in DEFAULT_OVERLAYS] + [MAIN]:
        if not os.path.exists(ov):
            continue
        root = manifest_root(ov)
        for line in open(ov):
            if (line.startswith("#") and "\t" not in line) or not line.strip():
                continue
            f = line.rstrip("\n").split("\t")
            if f[0] not in names or f[0] in won:
                continue
            won[f[0]] = {"manifest": ov, "root": root, "row": f}
            for c in LIB_COLS:
                if c < len(f) and f[c] not in ("-", ""):
                    lib = os.path.normpath(os.path.join(root, f[c]))
                    jobs.setdefault(lib, {"lib": lib, "stage": f[2], "names": []})["names"].append(f[0])
    missing = sorted(names - set(won))
    order = sorted(jobs)
    if sample:
        by_kind = {}
        for lib in order:
            by_kind.setdefault(source_kind(lib), []).append(lib)
        picked = []
        while len(picked) < sample and any(by_kind.values()):
            for k in sorted(by_kind):
                if by_kind[k] and len(picked) < sample:
                    picked.append(by_kind[k].pop(len(by_kind[k]) // 2))
        order = picked
    elif limit:
        order = order[:limit]
    p = {"names": len(names), "resolved": len(won), "unresolved": missing, "rows": won,
         "jobs": [jobs[l] for l in order], "created": time.time()}
    json.dump(p, open(os.path.join(work, "plan.json"), "w"))
    kinds = {}
    for j in p["jobs"]:
        kinds[source_kind(j["lib"])] = kinds.get(source_kind(j["lib"]), 0) + 1
    print("names %d, resolved %d, unresolved %d; libraries %d by source %s" % (len(names), len(won), len(missing),
                                                                               len(p["jobs"]), kinds))


def source_kind(lib):
    stem = lib[:-9] if lib.endswith(".metallib") else lib
    if os.path.exists(stem + ".msl"):
        return "msl"
    if os.path.exists(stem + ".metal.gz"):
        return "metal.gz"
    if os.path.exists(stem + ".json"):
        return "spirv"
    return "none"


def opts_of(r):
    opts = {"declared_output_variants": False}
    for p in r.get("passes", []):
        k = p["pass"]
        if k not in KNOWN_PASSES:
            raise RuntimeError("pass %s is not rebuilt by this tool" % k)
        if k == "x_buf_device":
            opts["x_buf_device"] = True
        elif k == "dual_source":
            opts["dual_source"] = True
        elif k == "output_variants":
            opts["output_variants"] = list(p["codes"])
        elif k == "lower_vertex_lane":
            opts["vs_route"] = "lane"
        elif k == "vertex_for_tessellation":
            opts["vs_route"] = "kernel"
        elif k == "drop_outputs":
            opts["drop_outputs"] = list(p["locations"])
        elif k == "shrink_lds":
            opts["shrink_lds"] = p["dwords"]
        elif k == "merge_typed_reads":
            opts["merge_typed_reads"] = True
        elif k == "loop_cap":
            opts["loop_cap"] = p["cap"]
        elif k == "trim_outputs":
            opts["trim_outputs"] = {int(a): b for a, b in p["widths"].items()}
    if r.get("object_key") or r.get("split_chunks"):
        raise RuntimeError("split mesh libraries are not rebuilt by this tool")
    return opts


def compile_msl(text, out_lib, tmp):
    base = os.path.join(tmp, os.path.basename(out_lib)[:-9])
    open(base + ".metal", "w").write(text)
    rc, o, e, dt = R.run([R.tool("metal"), "-std=" + R.RECIPE["std"], *R.RECIPE["metal"], base + ".metal", "-o",
                          base + ".air"], R.RECIPE["metal_timeout"])
    if rc != 0:
        raise RuntimeError("metal failed: " + ("TIMEOUT" if rc is None else (e or b"").decode("utf-8", "replace")[-600:]))
    part = out_lib + ".%d.part" % os.getpid()
    rc, o, e, dt2 = R.run([R.tool("metallib"), base + ".air", "-o", part], R.RECIPE["metal_timeout"])
    if rc != 0:
        raise RuntimeError("metallib failed")
    os.replace(part, out_lib)
    for ext in (".metal", ".air"):
        try:
            os.remove(base + ext)
        except OSError:
            pass
    return dt + dt2


def one(job, work):
    lib = job["lib"]
    key = hashlib.sha256(lib.encode()).hexdigest()[:24]
    done = os.path.join(work, "done", key + ".json")
    if os.path.exists(done):
        return json.load(open(done))
    if os.environ.get("REHASH_WAIT_FOR_GAME"):
        import subprocess
        while subprocess.run(["pgrep", "-x", "gta6rt"], capture_output=True).returncode == 0:
            time.sleep(5)
    t0 = time.monotonic()
    r = {"lib": lib, "names": job["names"], "source": source_kind(lib)}
    tmp = os.path.join(work, "tmp", str(os.getpid()))
    os.makedirs(tmp, exist_ok=True)
    try:
        stem = lib[:-9]
        if r["source"] in ("msl", "metal.gz"):
            text = open(stem + ".msl").read() if r["source"] == "msl" else gzip.open(stem + ".metal.gz", "rt").read()
            new, sites = R.hash_fmix32(text)
            r["sites"] = sites
            if sites:
                h = hashlib.sha256(new.encode()).hexdigest()
                out = os.path.join(work, "cas", h[:2], h + ".metallib")
                os.makedirs(os.path.dirname(out), exist_ok=True)
                if not os.path.exists(out):
                    r["compile_s"] = round(compile_msl(new, out, tmp), 2)
                if r["source"] == "msl":
                    open(out[:-9] + ".msl", "w").write(new)
                r["new_lib"] = out
        elif r["source"] == "spirv":
            meta = json.load(open(stem + ".json"))
            spv_path = spv_of(meta, job["names"])
            if not os.path.exists(spv_path):
                raise RuntimeError("no corpus SPIR-V " + spv_path)
            opts = opts_of(meta)
            if "output_variants" in opts:
                import out_types
                opts["output_signature"] = out_types.dxbc_targets(R.dxbc_of(spv_path))
            raw = open(spv_path, "rb").read()
            words = list(R.spv.struct.unpack("<%dI" % (len(raw) // 4), raw))
            if FNV_WORD not in words:
                r.update(status="ok", sites=0, wall_s=round(time.monotonic() - t0, 2))
                os.makedirs(os.path.dirname(done), exist_ok=True)
                json.dump(r, open(done + ".tmp", "w"))
                os.replace(done + ".tmp", done)
                return r
            w, passes, extra = R.prepare(words, meta["stage"], opts)
            translator = None
            for d in (SPIRV, os.path.dirname(spv_path)):
                if R.cache_key(raw, R.translator_id(d), passes, extra) == meta["key"]:
                    translator = R.translator_id(d)
                    break
            if translator is None:
                raise RuntimeError("the recorded options do not give the row's key back")
            opts["hash_kind"] = "fnv1a_fmix32"
            cas = os.path.join(work, "cas")
            nr = R.compile_one(spv_path, meta["stage"], cas, translator, opts, tmp=tmp)
            if nr.get("status") != "ok":
                raise RuntimeError("compile_one: %s %s" % (nr.get("status"), nr.get("error", "")[:300]))
            r["sites"] = nr.get("hash_fmix32_sites", 0)
            if r["sites"]:
                r["new_lib"] = os.path.join(cas, nr["key"][:2], nr["key"] + ".metallib")
                r["compile_s"] = round(nr.get("total_s", 0), 2)
            r["cached"] = bool(nr.get("cached"))
        else:
            raise RuntimeError("no MSL and no result row beside the metallib")
        r["status"] = "ok"
    except Exception as e:  # noqa: BLE001
        r["status"] = "fail"
        r["error"] = str(e)[:800]
    r["wall_s"] = round(time.monotonic() - t0, 2)
    os.makedirs(os.path.dirname(done), exist_ok=True)
    if r["status"] == "ok":
        json.dump(r, open(done + ".tmp", "w"))
        os.replace(done + ".tmp", done)
    return r


def build(work, jobs_n):
    p = json.load(open(os.path.join(work, "plan.json")))
    t0 = time.monotonic()
    results = []
    with open(os.path.join(work, "build.jsonl"), "a") as log, ProcessPoolExecutor(jobs_n) as ex:
        futs = [ex.submit(one, j, work) for j in p["jobs"]]
        for i, f in enumerate(as_completed(futs), 1):
            r = f.result()
            results.append(r)
            log.write(json.dumps(r) + "\n")
            log.flush()
            if i % 25 == 0 or i == len(futs):
                el = time.monotonic() - t0
                print("%d/%d in %.0f s (%.1f s a library; ETA %.0f s); failed %d" %
                      (i, len(futs), el, el / i, el / i * (len(futs) - i), sum(r["status"] != "ok" for r in results)),
                      flush=True)
    return results


def overlay(work):
    p = json.load(open(os.path.join(work, "plan.json")))
    done = {}
    for f in os.listdir(os.path.join(work, "done")):
        r = json.load(open(os.path.join(work, "done", f)))
        done[r["lib"]] = r
    fails = []
    if os.path.exists(os.path.join(work, "build.jsonl")):
        for line in open(os.path.join(work, "build.jsonl")):
            r = json.loads(line)
            if r["status"] != "ok" and r["lib"] not in done:
                fails.append(r)
    store = manifest_root(MAIN)
    head = "# shaderc_msl manifest v3 root=%s\n" % os.path.relpath(store, os.path.abspath(work))
    with open(MAIN) as f:
        f.readline()
        cols = f.readline()
    lines, changed_names, not_done = [], [], []
    for name, w in p["rows"].items():
        f = list(w["row"])
        changed = False
        for c in LIB_COLS:
            if c < len(f) and f[c] not in ("-", ""):
                lib = os.path.normpath(os.path.join(w["root"], f[c]))
                r = done.get(lib)
                if r is None:
                    not_done.append(lib)
                    continue
                if r.get("new_lib"):
                    f[c] = os.path.relpath(os.path.abspath(r["new_lib"]), store)
                    changed = True
                else:
                    f[c] = os.path.relpath(lib, store)
        if changed:
            f[1] = hashlib.sha256(("fmix32:" + f[1]).encode()).hexdigest()
            lines.append("\t".join(f) + "\n")
            changed_names.append(name)
    with open(os.path.join(work, "overlay.tsv"), "w") as o:
        o.write(head)
        o.write(cols)
        o.writelines(sorted(lines))
    rep = {"rows_changed": len(changed_names), "rows": len(p["rows"]), "libraries_done": len(done),
           "libraries_not_done": len(set(not_done)), "failed": [{k: r.get(k) for k in ("lib", "error")} for r in fails],
           "unresolved_names": p["unresolved"][:50], "unresolved": len(p["unresolved"]),
           "sites": sum(r.get("sites", 0) for r in done.values()),
           "libraries_with_sites": sum(1 for r in done.values() if r.get("sites"))}
    json.dump(rep, open(os.path.join(work, "report.json"), "w"), indent=1)
    print(json.dumps({k: v for k, v in rep.items() if k not in ("failed", "unresolved_names")}))
    return rep


def prebuild_manifest(out, overlay_path, sources):
    """The pcache manifest lines of SOURCES whose pipeline uses a shader OVERLAY has a row for (names may start
    with '#': a header line is one with no tab)."""
    names = set()
    for line in open(overlay_path):
        if line.strip() and not (line.startswith("#") and "\t" not in line):
            names.add(line.split("\t", 1)[0])
    head, seen, lines = [], set(), []
    for src in sources:
        for line in open(src):
            if line.startswith("#"):
                if len(head) < 2 and line not in head:
                    head.append(line)
                continue
            f = line.rstrip("\n").split("\t")
            if len(f) < 5 or f[0] in seen:
                continue
            used = {dec(x.split("=", 1)[1].rsplit("@", 1)[0]) for x in f[4].split() if re.match(r"(ms|ps|vs|cs|os)=", x)}
            if used & names:
                seen.add(f[0])
                lines.append(line if line.endswith("\n") else line + "\n")
    with open(out, "w") as fo:
        fo.writelines(head + lines)
    return len(lines), len(names)


def manifest(work):
    import subprocess
    n, rows = prebuild_manifest(os.path.join(work, "prebuild.manifest.txt"), os.path.join(work, "overlay.tsv"), [TIER])
    print("prebuild manifest: %d pipelines for %d rows" % (n, rows))
    ovs = [os.path.join(PORT, o) for o in reversed(DEFAULT_OVERLAYS) if os.path.exists(os.path.join(PORT, o))]
    cmd = [sys.executable, "-I", os.path.join(PORT, "d3d12_metal", "cache", "tools", "catalog_merge.py"), "--base", MAIN]
    for o in ovs + [os.path.join(work, "overlay.tsv")]:
        cmd += ["--overlay", o]
    subprocess.run(cmd + ["--out", os.path.join(work, "catalog.tsv")], check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["plan", "count", "build", "overlay", "manifest"])
    ap.add_argument("work")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--sample", type=int, default=0)
    ap.add_argument("--jobs", type=int, default=3)
    a = ap.parse_args()
    if a.cmd == "plan":
        plan(a.work, a.limit, a.sample)
    elif a.cmd == "build":
        res = build(a.work, a.jobs)
        return 1 if any(r["status"] != "ok" for r in res) else 0
    elif a.cmd == "overlay":
        rep = overlay(a.work)
        return 1 if rep["failed"] or rep["libraries_not_done"] else 0
    elif a.cmd == "manifest":
        manifest(a.work)
    elif a.cmd == "count":
        count(a.work)
    return 0


if __name__ == "__main__":
    sys.exit(main())
