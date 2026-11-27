#!/usr/bin/env python3
"""Bootstrap file for the Metal path's pipeline cache (pcache-bootstrap.txt beside the archive).

The first Metal run should already know which pipelines the title will use before the recorder has seen any. Two
sources that exist from the Vulkan path:
  V lines: PSO keys (sha256 of the D3D12 description, the Metal layer computes the same `pipeline_key`) from the
           Vulkan path's boot manifests (run/pipeline_cache*/**/boot_manifest.txt), in their first-use order.
  L lines: pipeline names ("VS,PS" or a compute name, as both layers label a PSO) from census lines of S3 runs:
           "census draw|dispatch skipped, its pipeline still building: NAME: n", attributed to the title's phase by
           the vk-counters time they first appear at and the log's "[host] stage t=" lines.
At PSO creation the layer matches the PSO's key or label and queues its build ahead of the first draw (at most 4
PSOs per label).

  bootstrap.py --port PORT --out ARCHIVE_DIR/pcache-bootstrap.txt [--logs GLOB]... [--manifests GLOB]...
"""
import argparse, glob, os, re, sys

PHASES = ["boot", "landing", "movie", "first-run", "story-load", "world"]
STAGE = re.compile(r"\[host\] stage t=([\d.]+)s: (.*?) \(threads")
COUNTERS_T = re.compile(r"\[rt\] vk-counters t=(\d+)s:")
STILL = re.compile(r"census (?:draw|dispatch) skipped, its pipeline still building: (.*): \d+$")


def phase_of_stage(text):
    if text.startswith("world"):
        return "world"
    if text.startswith("story load"):
        return "story-load"
    if text.startswith("first-run"):
        return "first-run"
    if text.startswith("landing"):
        return "landing"
    return "boot"


def census(path):
    stages, found, t = [], {}, None
    with open(path, errors="replace") as f:
        for line in f:
            m = STAGE.search(line)
            if m:
                stages.append((float(m.group(1)), phase_of_stage(m.group(2))))
                continue
            m = COUNTERS_T.search(line)
            if m:
                t = float(m.group(1))
                continue
            m = STILL.search(line.rstrip("\n"))
            if m:
                found.setdefault(m.group(1), t)
    out = {}
    for label, at in found.items():
        phase = "boot"
        if at is None:
            phase = stages[-1][1] if stages else "boot"
        else:
            for st, ph in stages:
                if st <= at:
                    phase = ph
        out[label] = phase
    return out


def vk_manifest(path):
    keys = []
    with open(path, errors="replace") as f:
        if f.readline().strip() != "# leonida boot manifest 1":
            return keys
        for line in f:
            p = line.split()
            if len(p) >= 2 and p[0] in ("g", "c"):
                keys.append(p[1])
    return keys


def manifest_phase(path):
    name = path.lower()
    if "s3" in name:
        return "story-load"
    if "libmoltenvk" in name:
        return "world"
    return "landing"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--logs", action="append")
    ap.add_argument("--manifests", action="append")
    a = ap.parse_args()
    logs = a.logs or [os.path.join(a.port, "run/logs", g) for g in ("s3*.log", "r4vk-s3-*.log", "gov-s3-*.log")]
    mans = a.manifests or [os.path.join(a.port, "run/pipeline_cache*/**/boot_manifest.txt")]
    rank = {p: i for i, p in enumerate(PHASES)}

    keys = {}
    for pattern in mans:
        for path in sorted(glob.glob(pattern, recursive=True)):
            ph = manifest_phase(path)
            n = 0
            for k in vk_manifest(path):
                n += 1
                if k not in keys or rank[ph] < rank[keys[k][0]]:
                    keys[k] = (ph, keys.get(k, (ph, len(keys)))[1])
            print(f"vulkan boot manifest {os.path.relpath(path, a.port)}: {n} keys as {ph}")
    labels = {}
    for pattern in logs:
        for path in sorted(glob.glob(pattern)):
            if path.endswith(".import.log"):
                continue
            got = census(path)
            for label, ph in got.items():
                if label not in labels or rank[ph] < rank[labels[label]]:
                    labels[label] = ph
            if got:
                print(f"census {os.path.relpath(path, a.port)}: {len(got)} names")
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    tmp = a.out + ".tmp"
    with open(tmp, "w") as f:
        f.write("# gta6rt pcache bootstrap 1 (cache/tools/bootstrap.py)\n# V <phase> <PSO key> | L <phase> <label>\n")
        for k, (ph, order) in sorted(keys.items(), key=lambda kv: (rank[kv[1][0]], kv[1][1])):
            f.write(f"V\t{ph}\t{k}\n")
        for label, ph in sorted(labels.items(), key=lambda kv: (rank[kv[1]], kv[0])):
            f.write(f"L\t{ph}\t{label}\n")
    os.replace(tmp, a.out)
    by = {}
    for ph, _ in keys.values():
        by[("V", ph)] = by.get(("V", ph), 0) + 1
    for ph in labels.values():
        by[("L", ph)] = by.get(("L", ph), 0) + 1
    print(f"wrote {a.out}: {len(keys)} PSO keys, {len(labels)} names; by phase: " +
          ", ".join(f"{k[0]} {k[1]} {v}" for k, v in sorted(by.items(), key=lambda kv: (kv[0][0], rank[kv[0][1]]))))


if __name__ == "__main__":
    sys.exit(main())
