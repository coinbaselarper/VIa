#!/usr/bin/env python3
"""The title's deterministic boot PSO set as pcache pipeline texts, for prepare_shaders --specs (no title run).

Sources (all already on disk):
  records  the package's pipeline library (shaders.rpf), read by port/shaderc_msl/tools/pipelines.py into
           build/shaderc_msl/pipelines/pairs.json: 25,471 graphics records (VS+PS, MS+PS, MS, VS) and 2,693 compute
           records. The title creates its slot-57 PSOs and its slot-67 compute PSOs from these. The graphics state
           blob is not decoded (leonida/docs/shader_library.md), so these specs carry a default state.
  census   the PSO census logs of the Vulkan path (run/logs/r4pso-{a,b,c}.log, spike/r4-pso-census: every graphics
           PSO creation with topology, target count, RT0/RT1/DSV formats, samples and RT0 blend; 72,019 calls, the
           same in all three runs): the boot set with its states, as far as the census logged them.
What a spec cannot know (input-layout formats, blend of targets 2-7, alpha factors) is filled the same way for
every spec: inputs from the vertex shader's input signature as 32-bit formats packed in slot 0, RT0's blend on
every target. So these specs measure the cost and size of the full set; an archive entry built from one serves the
title only where the guess equals the title's description (the recorder's manifest is exact).

  boot_set.py --port PORT --out DIR [--census GLOB]
writes DIR/records.specs, DIR/census.specs and DIR/boot_set.json (counts).
"""
import argparse, collections, glob, json, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pcache_text import render_text, mesh_text, compute_text, Target  # noqa: E402

GPSO = re.compile(r"\[r4\] gpso #(\d+) (\w+) .*? topo=(\d+) cons=(\d+) fill=(\d+) rts=(\d+) rt0=(\d+) rt1=(\d+) "
                  r"dsv=(\d+) samp=(\d+) ia=(\d+) blend=(\S+) refusal=(.*?) vs=(\S*) ps=(\S*)")
CPSO = re.compile(r"\[r4\] cpso #(\d+) (\w+) hr=\S+ id=\S+ bytes=\d+ (\S+)")


def load_catalog(path):
    out = {}
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                continue
            c = line.rstrip("\n").split("\t")
            if len(c) > 10:
                out[c[0]] = {"key": c[1], "stage": c[2], "mesh_route": c[10]}
    return out


def inputs_of(entry):
    out, off = [], 0
    for e in (entry or {}).get("input_signature", []):
        if e.get("system_value", 0):
            continue
        mask = e.get("mask", 15)
        n = max(i + 1 for i in range(4) if mask & (1 << i)) if mask else 4
        t = e.get("component_type", "float")
        if t in ("uint", "int"):
            fmt = {1: 42, 2: 36, 3: 7, 4: 3}[n] if t == "uint" else {1: 43, 2: 38, 3: 8, 4: 4}[n]
        else:
            fmt = {1: 41, 2: 16, 3: 6, 4: 2}[n]
        out.append((e["register"], fmt, 0, off))
        off += 4 * n
    return out


def depth_of(dsv):
    # The layer stores D24_UNORM_S8_UINT as 45 and other depth formats as their Metal-backed DXGI depth format.
    return {0: 0, 45: 45, 40: 40, 20: 20, 55: 55, 19: 20, 44: 45, 39: 40, 53: 55}.get(dsv, dsv)


def topology_of(t):
    return {1: 1, 2: 2, 3: 3, 19: 19, 32: 32}.get(t, 3)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--census", default=None)
    a = ap.parse_args()
    port = a.port
    root = os.path.dirname(port)
    cat = load_catalog(os.path.join(port, "build/shaderc_msl/runs/main/manifest.tsv"))
    index = json.load(open(os.path.join(root, "spike/s2-make-game/work/shaders/spirv/index.json")))
    lib_index = json.load(open(os.path.join(root, "spike/s2-make-game/work/shaders/spirv/library/index.json")))
    pairs = json.load(open(os.path.join(port, "build/shaderc_msl/pipelines/pairs.json")))
    os.makedirs(a.out, exist_ok=True)
    counts = collections.Counter()

    def ck(name):
        e = cat.get(name)
        return e["key"] if e else "-"

    rec = set()
    for g in pairs["graphics"]:
        vs, ps, ms = g.get("vs"), g.get("ps"), g.get("ms")
        if ms:
            counts["records mesh"] += 1
            e = cat.get(ms)
            if not e or (e["mesh_route"] not in ("-", "hw", "")):
                counts["records mesh not on the hw route (refused by the layer)"] += 1
                continue
            ls = index.get(ms, {}).get("local_size", [128, 1, 1])
            rec.add(mesh_text(ms, ck(ms), ps or "", ck(ps) if ps else "", ls, [Target(28)], 0, 1))
        elif vs:
            counts["records vs"] += 1
            rec.add(render_text(vs, ck(vs), ps or "", ck(ps) if ps else "", inputs_of(index.get(vs)), [Target(28)], 0, 1, 3))
        else:
            counts["records without vs/ms"] += 1
    for name, e in sorted(lib_index.items()):
        counts["records compute (library/)"] += 1
        n = "library/" + name
        rec.add(compute_text(n, ck(n), e.get("local_size", [0, 0, 0])))
    with open(os.path.join(a.out, "records.specs"), "w") as f:
        f.write("# pcache specs from the package's pipeline-library records (boot_set.py); default state\n")
        for t in sorted(rec):
            f.write("boot\t" + t + "\n")
    counts["records specs"] = len(rec)

    census_glob = a.census or os.path.join(port, "run/logs/r4pso-a.log")
    cen = []
    seen = set()
    for path in sorted(glob.glob(census_glob))[:1]:
        for line in open(path, errors="replace"):
            if not line.startswith("[r4] gpso"):
                continue
            m = GPSO.match(line)
            if not m:
                continue
            g = m.groups()
            counts["census graphics calls"] += 1
            if g[1] != "ok":
                continue
            counts["census graphics ok"] += 1
            topo, rts, rt0, rt1, dsv, samp = int(g[2]), int(g[5]), int(g[6]), int(g[7]), int(g[8]), int(g[9])
            vs, ps = g[13], g[14]
            if vs not in cat or (ps and ps not in cat):
                counts["census ok, a shader not in the store"] += 1
                continue
            bl = g[11].split("/")
            en, src, dst = (int(bl[0]), int(bl[1]), int(bl[2])) if len(bl) == 3 else (0, 2, 1)
            targets = []
            for i in range(max(rts, 0)):
                fmt = rt0 if i == 0 else rt1 if i == 1 and rt1 else rt0
                targets.append(Target(fmt, en, src, dst))
            t = render_text(vs, ck(vs), ps, ck(ps) if ps else "", inputs_of(index.get(vs)), targets, depth_of(dsv),
                            samp or 1, topology_of(topo))
            if t in seen:
                continue
            seen.add(t)
            cen.append(t)
        for line in open(path, errors="replace"):
            if line.startswith("[r4] cpso"):
                m = CPSO.match(line)
                if m:
                    counts["census compute calls"] += 1
                    counts["census compute ok"] += m.group(2) == "ok"
    with open(os.path.join(a.out, "census.specs"), "w") as f:
        f.write("# pcache specs from the PSO census (boot_set.py): distinct (shaders, logged state) of the ok creations\n")
        for t in cen:
            f.write("boot\t" + t + "\n")
    counts["census distinct specs"] = len(cen)
    json.dump(dict(counts), open(os.path.join(a.out, "boot_set.json"), "w"), indent=1)
    for k, v in counts.items():
        print(f"{k}: {v}")


if __name__ == "__main__":
    sys.exit(main())
