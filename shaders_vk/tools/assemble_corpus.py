#!/usr/bin/env python3
"""Assemble the complete Apple corpus (port/shaders_vk): every module the translator writes with --apple-float-controls
--vs-lane-from-ballot, from make_game's corpus (read-only) and whatever this translator copy has re-translated.

  assemble_corpus.py --base WORK/shaders/spirv --out OUT/spirv --retranslated DIR [--retranslated DIR ...]

Per module (main part and the exe, fspx, bvh, library sub-parts):
  - vertex shaders come from a re-translation (the lane rewrite changes their code); none found is an error.
  - every other stage is strip_float_controls.strip() of the base module. Where a re-translation of it exists, the two
    are compared byte for byte: identical ones are hard-linked from the re-translation, a difference is listed and the
    stripped bytes are used (a re-translation cut short by a killed shard ends mid-module).
A --retranslated DIR is a build_corpus.py output (its shaders_vk.json lists what it translated) or a bare folder of
.spv files from a translation shard (every .spv in it counts). Later DIRs win. index.json entries are strip_meta() of
the base entry, or the re-translation's own entry for vertex shaders. native_hashes.txt and wave64_emulate.txt are
copied, and OUT/../dxbc is filled with hard links to the base's containers (native_hashes.txt names ../dxbc/...).
OUT/shaders_vk.json records per-part counts by source and the comparison counts, not per-module sources:
corpus_provenance.py derives those from the hard links and checks them against the counts.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import strip_float_controls as sfc  # noqa: E402
import build_corpus  # noqa: E402

PARTS = ["", "exe", "fspx", "bvh", "library"]


def load_json(path):
    with open(path) as f:
        return json.load(f)


def retranslation_sources(dirs):
    """{part: {name: [(path, meta or None), ...]}} in the order of the dirs."""
    found = {p: {} for p in PARTS}
    for d in dirs:
        d = os.path.abspath(d)
        manifest = os.path.join(d, "shaders_vk.json")
        if os.path.isfile(manifest):
            m = load_json(manifest)
            for part, names in m.get("parts", {}).items():
                sub = "" if part == "main" else part
                folder = os.path.join(d, sub) if sub else d
                try:
                    index = load_json(os.path.join(folder, "index.json"))
                except OSError:
                    index = {}
                for n in names:
                    p = os.path.join(folder, n + ".spv")
                    if os.path.isfile(p):
                        found[sub].setdefault(n, []).append((p, index.get(n)))
        else:
            sub = os.path.basename(d.rstrip("/")) if os.path.basename(d.rstrip("/")) in PARTS[1:] else ""
            for f in os.listdir(d):
                if f.endswith(".spv"):
                    found[sub].setdefault(f[:-4], []).append((os.path.join(d, f), None))
    return found


def link_tree(src, dst):
    n = 0
    os.makedirs(dst, exist_ok=True)
    for f in os.listdir(src):
        s, t = os.path.join(src, f), os.path.join(dst, f)
        if os.path.isfile(s) and f.endswith(".dxbc") and not os.path.exists(t):
            os.link(s, t)
            n += 1
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True, help="make_game's spirv folder (read-only)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--retranslated", action="append", default=[])
    a = ap.parse_args()
    base, out = os.path.abspath(a.base), os.path.abspath(a.out)
    if os.path.exists(os.path.join(out, "index.json")):
        ap.error(f"{out} already holds a corpus")
    t0 = time.time()
    found = retranslation_sources(a.retranslated)
    report = {"translator": build_corpus.translator_digest(),
              "flags": build_corpus.BASE_FLAGS + build_corpus.APPLE_FLAGS, "base": base,
              "retranslated": [os.path.abspath(d) for d in a.retranslated], "parts": {}}
    errors = 0
    for part in PARTS:
        src = os.path.join(base, part) if part else base
        dst = os.path.join(out, part) if part else out
        index = load_json(os.path.join(src, "index.json"))
        os.makedirs(dst, exist_ok=True)
        out_index, counts, differ, sources = {}, {}, [], {}
        mine = found[part]
        for name, meta in index.items():
            stage = meta.get("stage")
            target = os.path.join(dst, name + ".spv")
            candidates = mine.get(name, [])
            with_meta = [c for c in candidates if c[1] is not None]
            re = with_meta[-1] if with_meta else (candidates[-1] if candidates else None)
            if stage == "vs":
                if not with_meta:
                    print(f"error: {part or 'main'}/{name}: vertex shader without an indexed re-translation",
                          file=sys.stderr)
                    errors += 1
                    continue
                with open(re[0], "rb") as f:
                    chosen = f.read()
                for other in candidates:
                    if other[0] != re[0]:
                        with open(other[0], "rb") as f:
                            same = f.read() == chosen
                        k = "vs: second re-translation identical" if same else "vs: second re-translation differs"
                        counts[k] = counts.get(k, 0) + 1
                        if not same:
                            differ.append(name)
                os.link(re[0], target)
                out_index[name] = re[1]
                key = "vs re-translated"
            else:
                with open(os.path.join(src, name + ".spv"), "rb") as f:
                    stripped = sfc.strip(f.read())
                key = "stripped"
                for c in candidates:
                    with open(c[0], "rb") as f:
                        theirs = f.read()
                    if theirs == stripped:
                        if not os.path.exists(target):
                            os.link(c[0], target)
                        key = "re-translated, identical to stripped"
                    else:
                        differ.append(name)
                        key = "re-translation differs, stripped used"
                        break
                if not os.path.exists(target):
                    with open(target, "wb") as f:
                        f.write(stripped)
                out_index[name] = sfc.strip_meta(meta)
                if re and re[1] is not None and re[1] != out_index[name]:
                    counts["index entry differs from re-translation"] = \
                        counts.get("index entry differs from re-translation", 0) + 1
            counts[key] = counts.get(key, 0) + 1
            sources[name] = key
        with open(os.path.join(dst, "index.json"), "w") as f:
            json.dump(out_index, f, sort_keys=True)
        for extra in ("native_hashes.txt", "wave64_emulate.txt"):
            if os.path.isfile(os.path.join(src, extra)):
                with open(os.path.join(src, extra), "rb") as f, open(os.path.join(dst, extra), "wb") as g:
                    g.write(f.read())
        report["parts"][part or "main"] = {"counts": counts, "differ": differ}
        print(f"{part or 'main'}: {len(out_index)} modules: {counts}" + (f"; differing: {differ[:5]}" if differ else ""))
    dxbc_src = os.path.join(os.path.dirname(base), "dxbc")
    if os.path.isdir(dxbc_src):
        n = link_tree(dxbc_src, os.path.join(os.path.dirname(out), "dxbc"))
        print(f"dxbc: {n} containers hard-linked beside {out}")
    with open(os.path.join(out, "translator_flags.txt"), "w") as f:
        f.write(" ".join(report["flags"]) + "\n")
    report["seconds"] = round(time.time() - t0, 1)
    with open(os.path.join(out, "shaders_vk.json"), "w") as f:
        json.dump(report, f, indent=1, sort_keys=True)
    print(f"done in {report['seconds']} s, {errors} errors")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
