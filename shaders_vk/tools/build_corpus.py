#!/usr/bin/env python3
"""Translate the title's shaders for Apple Vulkan drivers into a LEONIDA_SHADERS folder (port/shaders_vk).

Reads the carve/fssm/disassembly outputs of a make_game work folder (read-only) and runs this module's translator
copy (translator/, from spike/s2-make-game/tree) with --apple-float-controls --vs-lane-from-ballot on top of the
flags make_game used. Parts: main (shaders.rpf), exe, fspx, bvh, library; each part lands where the runtime's
ShaderLibrary reads it (<out>/index.json, <out>/<part>/index.json).

  build_corpus.py --work W --out O [--parts main,exe] [--names FILE] [--seed DIR] [--jobs 3]

--names translates only the listed programs (one per line, "exe/container_00" for a sub-part) and merges them into
the existing index of <out>. --seed hard-links an older corpus into an empty <out> first, so a partial
re-translation is still a complete folder; <out>/shaders_vk.json records which entries this translator made.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TRANSLATOR = os.path.join(os.path.dirname(HERE), "translator", "tools", "rdna2spirv.py")
BASE_FLAGS = ["--wave64-emulate", "--f16-interp-fallback"]
APPLE_FLAGS = ["--apple-float-controls", "--vs-lane-from-ballot"]
PARTS = {
    "main": ("shaders/dxbc/disasm", "", []),
    "exe": ("shaders/exe/disasm", "exe", []),
    "fspx": ("shaders/fspx/disasm", "fspx", []),
    "bvh": ("shaders/bvh/disasm", "bvh", ["--max-shared-memory", "32768"]),
    "library": ("shaders/library/disasm", "library", []),
}


def translator_digest():
    h = hashlib.sha256()
    tools = os.path.dirname(TRANSLATOR)
    for name in sorted(os.listdir(tools)):
        if name.endswith(".py"):
            with open(os.path.join(tools, name), "rb") as f:
                h.update(name.encode() + b"\0" + f.read())
    return h.hexdigest()


def run_shards(src, out, jobs, flags, log):
    os.makedirs(out, exist_ok=True)
    py = sys.executable
    procs = []
    for i in range(jobs):
        lf = open(os.path.join(out, f"translate.shard{i}.log"), "w")
        procs.append((subprocess.Popen([py, "-I", TRANSLATOR, src, "--out", out, "--shard", f"{i}/{jobs}",
                                        "--no-asm", *flags], stdout=lf, stderr=subprocess.STDOUT), lf))
    bad = 0
    for p, lf in procs:
        bad += p.wait() != 0
        lf.close()
    r = subprocess.run([py, "-I", TRANSLATOR, src, "--out", out, "--merge"], capture_output=True, text=True)
    log(f"  merge rc={r.returncode} {r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ''}")
    if r.returncode != 0:
        log(r.stderr)
        return False
    return bad == 0


def stage_names(src, names, stage):
    disasm = os.path.join(stage, "disasm")
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(disasm)
    missing = []
    for n in names:
        s = os.path.join(src, n + ".s")
        if os.path.isfile(s):
            os.symlink(s, os.path.join(disasm, n + ".s"))
        else:
            missing.append(n)
    return disasm, missing


def seed(old, out, log):
    if os.path.exists(os.path.join(out, "index.json")):
        return
    t0 = time.time()
    n = 0
    for root, dirs, files in os.walk(old):
        dirs[:] = [d for d in dirs if d != "stubs"]
        rel = os.path.relpath(root, old)
        dst = os.path.join(out, rel) if rel != "." else out
        os.makedirs(dst, exist_ok=True)
        for f in files:
            if f.endswith(".spv"):
                os.link(os.path.join(root, f), os.path.join(dst, f))
                n += 1
            elif f == "index.json":
                shutil.copyfile(os.path.join(root, f), os.path.join(dst, f))
    log(f"seeded {n} modules from {old} by hard link in {time.time() - t0:.1f} s")


def merge_into(out_part, staged, names_done, move=False):
    with open(os.path.join(staged, "index.json")) as f:
        new = json.load(f)
    path = os.path.join(out_part, "index.json")
    try:
        with open(path) as f:
            index = json.load(f)
    except OSError:
        index = {}
    os.makedirs(out_part, exist_ok=True)
    for name, meta in new.items():
        dst = os.path.join(out_part, name + ".spv")
        if os.path.lexists(dst):
            os.unlink(dst)
        (os.replace if move else shutil.copyfile)(os.path.join(staged, name + ".spv"), dst)
        index[name] = meta
        names_done.append(name)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(index, f, sort_keys=True)
    os.replace(tmp, path)
    return len(new)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True, help="make_game work folder (read-only)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--parts", default="main,exe,fspx,bvh,library")
    ap.add_argument("--names", help="file with program names to translate (sub-parts as part/name)")
    ap.add_argument("--seed", help="older corpus to hard-link into an empty --out first")
    ap.add_argument("--jobs", type=int, default=3)
    ap.add_argument("--no-apple", action="store_true", help="translate with make_game's flags only")
    a = ap.parse_args()
    if a.jobs > 3:
        ap.error("at most 3 jobs on this machine")
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    logf = open(os.path.join(out, "build_corpus.log"), "a")

    def log(msg):
        line = f"[{time.strftime('%H:%M:%S')}] {msg}"
        print(line, flush=True)
        logf.write(line + "\n")
        logf.flush()

    flags_extra = [] if a.no_apple else APPLE_FLAGS
    digest = translator_digest()
    manifest_path = os.path.join(out, "shaders_vk.json")
    try:
        with open(manifest_path) as f:
            manifest = json.load(f)
    except OSError:
        manifest = {"translator": digest, "flags": BASE_FLAGS + flags_extra, "parts": {}}
    if manifest.get("translator") != digest or manifest.get("flags") != BASE_FLAGS + flags_extra:
        log("translator or flags differ from the manifest: entries made before are listed under 'stale'")
        manifest.setdefault("stale", {}).update(manifest.get("parts", {}))
        manifest.update({"translator": digest, "flags": BASE_FLAGS + flags_extra, "parts": {}})
    if a.seed:
        seed(os.path.abspath(a.seed), out, log)
        manifest["seed"] = os.path.abspath(a.seed)

    wanted = None
    if a.names:
        wanted = {}
        with open(a.names) as f:
            for line in f:
                n = line.strip()
                if not n:
                    continue
                part, _, rest = n.partition("/")
                if rest and part in PARTS and part != "main":
                    wanted.setdefault(part, []).append(rest)
                else:
                    wanted.setdefault("main", []).append(n)

    for part in a.parts.split(","):
        rel_src, sub, part_flags = PARTS[part]
        src = os.path.join(os.path.abspath(a.work), rel_src)
        out_part = os.path.join(out, sub) if sub else out
        flags = BASE_FLAGS + part_flags + flags_extra
        t0 = time.time()
        done = manifest["parts"].setdefault(part, [])
        if wanted is not None:
            names = sorted(set(wanted.get(part, [])) - set(done))
            if not names:
                continue
            stage = os.path.join(out, ".stage", part)
            disasm, missing = stage_names(src, names, stage)
            if missing:
                log(f"{part}: {len(missing)} names have no disassembly: {missing[:5]}")
            staged = os.path.join(stage, "spv")
            log(f"{part}: translating {len(names) - len(missing)} named programs with {' '.join(flags)}")
            ok = run_shards(disasm, staged, min(a.jobs, max(1, len(names) // 8)), flags, log)
            n = merge_into(out_part, staged, done) if os.path.isfile(os.path.join(staged, "index.json")) else 0
            shutil.rmtree(stage, ignore_errors=True)
        else:
            log(f"{part}: translating {src} with {' '.join(flags)}")
            staged = os.path.join(out, ".stage", part + "-all")
            ok = run_shards(src, staged, a.jobs, flags, log)
            n = merge_into(out_part, staged, done, move=True) if os.path.isfile(os.path.join(staged, "index.json")) else 0
            for f in os.listdir(staged):
                if f.endswith(".log"):
                    shutil.copyfile(os.path.join(staged, f), os.path.join(out, f"{part}.{f}"))
            shutil.rmtree(staged, ignore_errors=True)
        manifest["parts"][part] = sorted(set(done))
        tmp = manifest_path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(manifest, f, indent=1, sort_keys=True)
        os.replace(tmp, manifest_path)
        log(f"{part}: {n} modules in {time.time() - t0:.0f} s (shards ok={ok})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
