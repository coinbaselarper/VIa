#!/usr/bin/env python3
"""Geometry-shader census: is any of the 195 GS (or the voxelisation family around them) referenced by a pipeline?

Evidence sources, all offline:
  1. the title's pipeline library (pairs.json from tools/pipelines.py): every graphics record's stage slots,
     resolved to carved containers by HASH digest
  2. the stage slots the library uses at all (a GS slot would show up as a used slot other than 0/1/2/6)
  3. the two-payload VS containers (VS built as the export stage of a legacy GS pipeline) and the VS/PS/CS of the
     same family, looked up in the same records
  4. the executable: references to the family's names
  5. the title runs' logs (port/run/logs): shaders named by pipeline-compile-begin lines (what the runtime actually
     compiled), and the runtime's PSO counters
Writes <out>/gs_census.json.
"""
import argparse, collections, json, os, re, sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--spirv-dir", required=True)
    ap.add_argument("--pairs", required=True)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--logs", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "run", "logs"))
    a = ap.parse_args()
    index = json.load(open(os.path.join(a.spirv_dir, "index.json")))
    pairs = json.load(open(a.pairs))
    alias = pairs.get("digest_aliases", {})
    used = collections.defaultdict(set)
    slots = collections.Counter()
    for g in pairs["graphics"]:
        for k in g:
            if k.endswith("_program"):
                slots[k[:-8]] += 1
        for s in ("ps", "vs", "ms", "slot6"):
            n = g.get(s)
            if n:
                for x in [n] + alias.get(n, []):
                    used[s].add(x)
    for c in pairs["compute"]:
        if c.get("cs"):
            for x in [c["cs"]] + alias.get(c["cs"], []):
                used["cs"].add(x)
    allused = set().union(*used.values())
    gs = sorted(k for k, v in index.items() if v["stage"] == "gs")
    fam = collections.defaultdict(list)
    for k, v in index.items():
        b = re.sub(r"_[0-9a-f]{8}_Wrapped(~\d+)?$", "", k)[1:]
        if "Voxeliz" in b:
            fam[(v["stage"], b)].append(k)
    gs_hashes = {re.search(r"_([0-9a-f]{8})_Wrapped", k).group(1) for k in gs}
    same_hash = collections.Counter()
    for k, v in index.items():
        m = re.search(r"_([0-9a-f]{8})_Wrapped", k)
        if m and m.group(1) in gs_hashes and v["stage"] != "gs":
            same_hash[(v["stage"], k in allused)] += 1
    exe = open(a.exe, "rb").read()
    names = sorted({b for (_, b) in fam})
    exe_hits = {n: exe.count(n.encode()) for n in names}
    exe_hits["Voxeliz"] = exe.count(b"Voxeliz")
    exe_hits["EntityCoarseVoxelization"] = exe.count(b"EntityCoarseVoxelization")
    import glob
    rt_names, rt_lines, rt_files, psos = collections.Counter(), 0, 0, {}
    for f in sorted(glob.glob(os.path.join(a.logs, "*.log"))):
        hit = False
        for line in open(f, errors="replace"):
            if "pipeline-compile-begin" in line:
                hit = True
                rt_lines += 1
                m = re.search(r"shaders=(\S+)", line)
                for n in m.group(1).split(",") if m else []:
                    rt_names[n] += 1
            m = re.search(r"vk-counters t=\d+s: psos graphics (\d+) ok (\d+) failed, compute (\d+) ok (\d+) failed", line)
            if m:
                psos[os.path.basename(f)] = [int(x) for x in m.groups()]
        rt_files += hit
    sub = {}
    for d in ("library", "exe", "fspx", "bvh"):
        p = os.path.join(a.spirv_dir, d, "index.json")
        if os.path.exists(p):
            for k, v in json.load(open(p)).items():
                sub[d + "/" + k] = v["stage"]
    rt_stage = collections.Counter(index[n]["stage"] if n in index else sub.get(n, "unknown") for n in rt_names)
    res = {
        "gs_shaders": len(gs),
        "gs_in_library_records": sum(1 for k in gs if k in allused),
        "library_records": {"graphics": len(pairs["graphics"]), "compute": len(pairs["compute"])},
        "stage_slots_used": dict(slots),
        "voxelize_family": {f"{st} {b}": {"shaders": len(v), "in_library_records": sum(1 for k in v if k in allused)}
                            for (st, b), v in sorted(fam.items())},
        "same_hash_suffix_as_a_gs": {f"{st} in_library={u}": n for (st, u), n in sorted(same_hash.items())},
        "exe_string_hits": exe_hits,
        "library_programs_without_source": pairs.get("digest_unmatched"),
        "slot6_records": [g for g in pairs["graphics"] if "slot6_program" in g],
        "runtime_logs": {"files_with_pipeline_compiles": rt_files, "pipeline_compile_lines": rt_lines,
                         "distinct_shaders_by_stage": dict(rt_stage),
                         "gs_named": sorted(n for n in rt_names if n in index and index[n]["stage"] == "gs"),
                         "last_pso_counters_by_log [gfx ok, gfx failed, cs ok, cs failed]": psos},
    }
    os.makedirs(a.out, exist_ok=True)
    json.dump(res, open(os.path.join(a.out, "gs_census.json"), "w"), indent=1)
    print(json.dumps(res, indent=1))


if __name__ == "__main__":
    sys.exit(main())
