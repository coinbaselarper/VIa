#!/usr/bin/env python3
"""Inputs for tests/vs_lane_test: real corpus vertex shaders that load the subgroup lane id, in both lowerings.

  lane_mut.metal  negative control: lane id reversed within the SIMD group (31 - lane)
  lane.metal    recipe route "lane" (SPIR-V pre-pass, simd_prefix_exclusive_sum lane id, a vertex function),
                plus a capture store of main0_out to buffer(28) at the vertex id
  kernel.metal  --msl-vertex-for-tessellation (a kernel; SPIRV-Cross already stores main0_out to buffer(28))
manifest.txt (read by the test):
  shader <stem> <dir> <tier> <push_index_lane> <push_index_kernel> <simd ops>
  attr <stem> <index> <type>
Selection: vertex shaders with no descriptor bindings whose SPIR-V loads the lane id, every one of them unless
--limit, ordered by tier and name.
"""
import argparse, json, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shaderc_msl as S  # noqa: E402
import spv  # noqa: E402


def capture(msl):
    m = re.search(r"^vertex main0_out main0\((.*)\)\n\{", msl, re.M)
    if not m:
        raise ValueError("no vertex entry")
    params = m.group(1)
    vid = re.search(r"uint (\w+) \[\[vertex_id\]\]", params)
    extra = ", device main0_out* spvCapture [[buffer(28)]]"
    name = vid.group(1) if vid else "spvCapVid"
    if not vid:
        extra += ", uint spvCapVid [[vertex_id]]"
    head = f"vertex main0_out main0({params}{extra})\n{{"
    body_start = m.end()
    rest = msl[body_start:]
    n = rest.count("return out;")
    if n == 0:
        raise ValueError("no return out")
    rest = rest.replace("return out;", f"{{ spvCapture[{name}] = out; return out; }}")
    return msl[:m.start()] + head + rest, n


def attrs(msl):
    body = S.struct_body(msl, "main0_in") or ""
    return [(int(a.group(3)), a.group(1)) for a in re.finditer(r"(\w+) (\w+) \[\[attribute\((\d+)\)\]\];", body)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--spirv-dir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--stems", default="", help="comma list: only these shaders")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    index = json.load(open(os.path.join(a.spirv_dir, "index.json")))
    cands = []
    for k in sorted(index):
        v = index[k]
        if v["stage"] != "vs" or v.get("bindings"):
            continue
        if not all(r["kind"] in ("const32", "ptr32_lo") for r in v.get("user_sgpr_reads", [])):
            continue
        w = spv.load(os.path.join(a.spirv_dir, k + ".spv"))
        if S.vs_lane_used(w):
            cands.append((v.get("wave64_graphics", "?"), k))
    cands.sort()
    if a.stems:
        want = set(a.stems.split(","))
        cands = [c for c in cands if any(c[1].endswith(w) or w in c[1] for w in want)]
    total = len(cands)
    if a.limit:
        step = max(1, len(cands) // a.limit)
        cands = cands[::step][:a.limit]
    lines = []
    skipped = []
    for tier, stem in cands:
        d = os.path.join(a.out, re.sub(r"[^A-Za-z0-9_]", "_", stem))
        os.makedirs(d, exist_ok=True)
        words = spv.load(os.path.join(a.spirv_dir, stem + ".spv"))
        try:
            w, passes, _ = S.prepare(words, "vs", {"vs_route": "lane"})
            spv.save(w, os.path.join(d, "lane.spv"))
            rc, out, err, _ = S.run([S.SPIRV_CROSS, os.path.join(d, "lane.spv"), *S.RECIPE["cross"], "--msl-version", "30100"], 300)
            if rc != 0:
                raise ValueError("lane cross: " + err.decode()[:200])
            lt = out.decode()
            lane, nret = capture(S.msl_fixups(lt, passes)[0] or lt)
            w2, _ = spv.split_bindings(words)
            spv.save(w2, os.path.join(d, "kernel.spv"))
            rc, out2, err, _ = S.run([S.SPIRV_CROSS, os.path.join(d, "kernel.spv"), *S.RECIPE["cross"], "--msl-version", "30100",
                                      "--msl-vertex-for-tessellation"], 300)
            if rc != 0:
                raise ValueError("kernel cross: " + err.decode()[:200])
            kern = out2.decode()
            kern = S.msl_fixups(kern, passes)[0] or kern
        except ValueError as e:
            skipped.append((stem, str(e)))
            continue
        open(os.path.join(d, "lane.metal"), "w").write(lane)
        open(os.path.join(d, "lane_mut.metal"), "w").write(lane.replace("simd_prefix_exclusive_sum(1u)", "(31u - simd_prefix_exclusive_sum(1u))"))
        open(os.path.join(d, "kernel.metal"), "w").write(kern)
        pl, pk = S.msl_plan(lane), S.msl_plan(kern)
        ops = sorted(set(re.findall(r"\b(simd_\w+|quad_\w+)\(", lane)))
        lines.append(f"shader {stem} {d} {tier} {pl['push']} {pk['push']} {','.join(ops) or '-'}")
        for i, t in attrs(lane):
            lines.append(f"attr {stem} {i} {t}")
    open(os.path.join(a.out, "manifest.txt"), "w").write("\n".join(lines) + "\n")
    json.dump({"candidates": total, "selected": len(cands), "written": sum(1 for l in lines if l.startswith("shader")), "skipped": skipped},
              open(os.path.join(a.out, "gen.json"), "w"), indent=1)
    print(f"vs_lane_gen: {total} candidates, {len(cands)} selected, {sum(1 for l in lines if l.startswith('shader'))} written, {len(skipped)} skipped")


if __name__ == "__main__":
    sys.exit(main())
