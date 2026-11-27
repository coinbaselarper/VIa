#!/usr/bin/env python3
"""The compute shaders that built no pipeline within 300 s in spike/s5.2: front-end variants vs device-compile time.

For each shader: recipe MSL, then metallibs from `metal -c` at default optimisation and at -O0, then a compute PSO
from each with a long timeout (one at a time). Writes <out>/huge_cs.jsonl.
"""
import argparse, json, os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import probe as P  # noqa: E402
import shaderc_msl as S  # noqa: E402
import spv  # noqa: E402

HUGE = ["Artdirectionalshadows_CS_RTShadowTrace32HQ_Tiled_bfd3b062_Wrapped",
        "Hindirect_diffuse_lighting_CS_ScreenProbes_ActivateRays_d60186e6_Wrapped",
        "Irtdirectionalshadows_CS_RTShadowTrace32TemporalHQ_Tiled_bfd3b062_Wrapped",
        "Jindirect_diffuse_lighting_CS_ScreenProbes_ActivateProbes_9083c094_Wrapped",
        "Pindirect_diffuse_lighting_CS_ScreenProbes_ActivateProbesTarget_534f271f_Wrapped",
        "Rcascadeshadows_rendering_CS_ShadowRenderTiledCascadesAlphaCBPPPC_9945271c_Wrapped",
        "Urtdirectionalshadows_CS_RTShadowTrace32TemporalHQ_OneOver2RPP_Tiled_bfd3b062_Wrapped"]


def noinline(msl, min_calls):
    return S.noinline_hot(msl, min_calls)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--spirv-dir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", default="")
    ap.add_argument("--opts", default="default,O0")
    ap.add_argument("--pso-timeout", type=float, default=1200)
    ap.add_argument("--noinline-min-calls", type=int, default=0)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    names = [n for n in HUGE if not a.only or a.only in n]
    with open(os.path.join(a.out, "huge_cs.jsonl"), "a") as fo:
        for n in names:
            w, passes, extra = S.prepare(spv.load(os.path.join(a.spirv_dir, n + ".spv")), "cs", {})
            base = os.path.join(a.out, n[:40])
            spv.save(w, base + ".spv")
            rc, _, err, tc = S.run([S.SPIRV_CROSS, base + ".spv", *S.RECIPE["cross"], "--msl-version", "30100", "--output", base + ".metal"], 300)
            ni = 0
            if a.noinline_min_calls:
                t, ni = noinline(open(base + ".metal").read(), a.noinline_min_calls)
                open(base + ".metal", "w").write(t)
            for opt in a.opts.split(","):
                row = {"stem": n, "opt": opt, "noinline_functions": ni, "spv_bytes": os.path.getsize(base + ".spv"), "cross_s": round(tc, 1),
                       "msl_bytes": os.path.getsize(base + ".metal")}
                flags = [] if opt == "default" else ["-" + opt]
                rc, _, err, tm = S.run([S.tool("metal"), "-std=metal3.1", "-c", "-w", *flags, base + ".metal", "-o", base + ".air"], 1800)
                row["metal_s"] = round(tm, 1)
                row["metal_rss_mb"] = round(S.PEAK["last"] / 2**20)
                if rc != 0:
                    row["result"] = "METAL_FAIL"
                    fo.write(json.dumps(row) + "\n")
                    fo.flush()
                    continue
                S.run([S.tool("metallib"), base + ".air", "-o", base + "." + opt + ".metallib"], 600)
                row["metallib_bytes"] = os.path.getsize(base + "." + opt + ".metallib")
                pr = P.Probe()
                t0 = time.monotonic()
                j = pr.run({"mode": "cs", "lib": os.path.abspath(base + "." + opt + ".metallib"), "fn": "main0"}, timeout=a.pso_timeout)
                pr.close()
                row.update(result=j.get("result"), pso_s=round(time.monotonic() - t0, 1), error=j.get("error", "")[:200])
                fo.write(json.dumps(row) + "\n")
                fo.flush()
                print(json.dumps(row), flush=True)
            for e in (".spv", ".air"):
                if os.path.exists(base + e):
                    os.remove(base + e)


if __name__ == "__main__":
    sys.exit(main())
