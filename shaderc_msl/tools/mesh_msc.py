#!/usr/bin/env python3
"""Mesh-as-compute variants: the translator's --mesh-compute module (the mesh program as a compute kernel that writes
its counts, vertices and primitives to a buffer) plus the vertex shader that draws them (mesh_compute_vs.py), both
built with the shaderc_msl recipe into the content-addressed store.

  mesh_msc.py --stems FILE --work DIR --build port/build/shaderc_msl --out msc.jsonl [--jobs 1]

Row: stem, cs_key, vs_key, status, layout (the translator's "mesh_compute" record: slot bytes, vertex stride, param
offsets, primitive offset, address dwords). The compute kernel and the vertex shader take the buffer address in push
dwords 36..37 (push block 152 bytes); one draw of groups * max_primitives * 3 non-indexed vertices follows the
dispatch (mesh_compute_vs.py header).
"""
import argparse, json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import shaderc_msl as S  # noqa: E402
import mesh_split_check as C  # noqa: E402

MCVS = os.path.join(os.path.dirname(C.TRANSLATOR), "mesh_compute_vs.py")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stems", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--build", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    a.work = os.path.abspath(a.work)
    stems = [x.strip() for x in open(a.stems) if x.strip()]
    ref_dir, index = C.make_refs(stems, a.work)
    r = subprocess.run([C.PY314, MCVS, ref_dir], capture_output=True, text=True)
    if r.returncode:
        sys.exit("mesh_compute_vs: " + r.stderr[-400:])
    tr = S.translator_id(os.path.join(C.CORPUS, "spirv"))
    tr = dict(tr, mesh_compute=True)
    cas = os.path.join(a.build, "cas")
    tmp = os.path.join(a.build, "tmp", "msc")
    os.makedirs(tmp, exist_ok=True)
    done = set()
    if os.path.exists(a.out):
        done = {json.loads(l)["stem"] for l in open(a.out)}
    for stem in stems:
        if stem in done:
            continue
        row = {"stem": stem}
        try:
            lay = index[stem]["mesh_compute"]
            cs = S.compile_one(os.path.join(ref_dir, stem + ".spv"), "cs", cas, tr, {"vs_route": "lane"}, tmp=tmp)
            vs = S.compile_one(os.path.join(ref_dir, stem + ".mesh_vs.spv"), "vs", cas, tr, {"vs_route": "lane"}, tmp=tmp)
            row.update(cs_key=cs.get("key"), cs_status=cs["status"], vs_key=vs.get("key"), vs_status=vs["status"],
                       layout=lay, local_size=index[stem]["local_size"], f16_halves=index[stem].get("f16_halves"),
                       cs_push=(cs.get("plan") or {}).get("push"), vs_push=(vs.get("plan") or {}).get("push"),
                       cs_sets=(cs.get("plan") or {}).get("sets"), vs_sets=(vs.get("plan") or {}).get("sets"),
                       cs_error=cs.get("error"), vs_error=vs.get("error"),
                       lds_bytes=index[stem].get("lds_bytes"))
            row["status"] = "ok" if cs["status"] == "ok" and vs["status"] == "ok" else "fail"
        except Exception as e:  # noqa: BLE001
            row["status"] = "error"
            row["error"] = str(e)[:300]
        print(json.dumps({k: v for k, v in row.items() if k != "layout"}), flush=True)
        open(a.out, "a").write(json.dumps(row) + "\n")


if __name__ == "__main__":
    main()
