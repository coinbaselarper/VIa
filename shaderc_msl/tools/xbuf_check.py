#!/usr/bin/env python3
"""Does moving the wave64 exchange buffer to device memory (cs_xbuf.py) change what a compute shader does?

For corpus compute shaders that fit in threadgroup memory either way (no descriptor tables, wave64-emulated), build
the shader twice (as translated, and with x_buf_device), run both on the same patterned heap and push constants and
compare a digest of the whole heap after the run (every output goes through physical-storage-buffer stores into it).

  xbuf_check.py --build port/build/shaderc_msl --n 6 --groups 2 --out xbuf_check.jsonl
"""
import argparse, json, os, random, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import shaderc_msl as S  # noqa: E402
import mesh_split_check as C  # noqa: E402

RUN = os.path.join(C.ROOT, "port/build/shaderc_msl/bin2/mesh_run_cs")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--n", type=int, default=6)
    ap.add_argument("--groups", type=int, default=2)
    ap.add_argument("--pattern", default="rngkb:16384:16388")
    ap.add_argument("--heap-mb", type=int, default=512)
    ap.add_argument("--out", required=True)
    ap.add_argument("--seed", type=int, default=3)
    a = ap.parse_args()
    index = json.load(open(os.path.join(C.CORPUS, "spirv", "index.json")))
    cands = [k for k, v in index.items() if v["stage"] == "cs" and not v.get("bindings") and v.get("wave64_emulated")
             and v.get("wave64_exchange") == "workgroup" and v.get("lds_bytes", 0) < 20000
             and v["local_size"][0] * v["local_size"][1] * v["local_size"][2] <= 256 and "/" not in k]
    random.seed(a.seed)
    pick = random.sample(sorted(cands), min(a.n, len(cands)))
    tr = S.translator_id(os.path.join(C.CORPUS, "spirv"))
    S.tool_versions()
    cas = os.path.join(a.build, "cas")
    h = C.Harness()
    h.p = None
    import subprocess as sp

    def start():
        h.p = sp.Popen([RUN], stdin=sp.PIPE, stdout=sp.PIPE, text=True, bufsize=1)

    h.start = start
    for stem in pick:
        row = {"stem": stem}
        try:
            ls = index[stem]["local_size"]
            base = S.compile_one(os.path.join(C.CORPUS, "spirv", stem + ".spv"), "cs", cas, tr, {"vs_route": "lane"},
                                 tmp=os.path.join(a.build, "tmp", "xbc"))
            xb = S.compile_one(os.path.join(C.CORPUS, "spirv", stem + ".spv"), "cs", cas, tr,
                               {"vs_route": "lane", "x_buf_device": True}, tmp=os.path.join(a.build, "tmp", "xbc"))
            if base["status"] != "ok" or xb["status"] != "ok":
                raise RuntimeError("build " + str(base.get("error") or xb.get("error")))
            lib = lambda r: os.path.join(cas, r["key"][:2], r["key"] + ".metallib")
            j0 = {"mode": "cs", "lib": lib(base), "fn": "main0", "groups": [a.groups, 1, 1], "threads": ls, "push_bytes": 144,
                  "push_index": base["plan"]["push"], "pattern": a.pattern, "seed": a.seed, "heap_mb": a.heap_mb}
            j1 = dict(j0, lib=lib(xb), push_index=xb["plan"]["push"], xb_index=xb["plan"]["x_buf_buffer"],
                      xb_bytes=[p for p in xb["passes"] if p["pass"] == "x_buf_device"][0]["x_buf_words"] * 4 * a.groups)
            h.job(dict(j0, warm=True), gate=False)
            h.job(dict(j1, warm=True), gate=False)
            r0 = h.job(j0, timeout=60)
            r1 = h.job(j1, timeout=60)
            row.update(local_size=ls, base=r0, xbuf=r1)
            if r0.get("result") == "OK" and r1.get("result") == "OK":
                row["heap_changed_by_run"] = r0["heap_before"] != r0["heap_after"]
                row["verdict"] = "EQUAL" if r0["heap_after"] == r1["heap_after"] else "DIFFER"
            else:
                row["verdict"] = "RUN_FAIL"
        except Exception as e:  # noqa: BLE001
            row["verdict"] = "ERROR"
            row["error"] = str(e)[:300]
        print(json.dumps(row), flush=True)
        open(a.out, "a").write(json.dumps(row) + "\n")


if __name__ == "__main__":
    main()
