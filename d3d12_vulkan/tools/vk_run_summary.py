#!/usr/bin/env python3
"""One summary line per run log of the title on the Vulkan path (run_vk.sh): first lit frame, PSO and pipeline counts,
build time, ExecuteCommandLists stalls, skipped uses, session pipeline cache. Reads the "[rt] vk-counters" lines.

  vk_run_summary.py LOG [LOG...]
"""
import re
import sys

COUNTERS = re.compile(r"\[rt\] vk-counters t=(\d+)s: (.*)")


def field(text, pattern, cast=float, default=None):
    m = re.search(pattern, text)
    return cast(m.group(1)) if m else default


def summary(path):
    lines = open(path, errors="replace").read().splitlines()
    first = next((l for l in lines if re.search(r"first non-black frame \d+ at t=", l)), None)
    lit = field(first or "", r"at t=([\d.]+)s") if first else None
    reports = [(int(m.group(1)), m.group(2)) for m in map(COUNTERS.match, lines) if m]
    out = {"log": path.rsplit("/", 1)[-1], "first_lit_s": lit}
    if reports:
        t, last = reports[-1]
        out.update({
            "t": t,
            "pso_gfx_ok": field(last, r"psos graphics (\d+) ok", int),
            "pso_gfx_failed": field(last, r"psos graphics \d+ ok (\d+) failed", int),
            "pso_cs_ok": field(last, r"compute (\d+) ok", int),
            "vk_gfx": field(last, r"vk pipelines graphics (\d+) built", int),
            "vk_cs": field(last, r"compute (\d+) built", int),
            "build_s": field(last, r"([\d.]+) s building"),
            "longest_build_s": field(last, r"longest ([\d.]+) s,"),
            "cache_hits": field(last, r"(\d+) cache hits", int),
            "worker_builds": field(last, r"on the compiler thread (\d+) builds", int),
            "submit_longest_s": field(last, r"ExecuteCommandLists \(longest ([\d.]+) s", float),
            "submits_over_1s": field(last, r"(\d+) over 1 s", int),
            "not_ready": field(last, r"pipelines not ready at use (\d+)", int),
            "draws": field(last, r"draws (\d+) emitted", int),
            "draws_skipped": field(last, r"emitted (\d+) skipped", int),
            "draws_pending": field(last, r"\((\d+) of them for a pipeline not built yet", int),
            "null_builds": field(last, r"(\d+) compiler-thread builds without a pipeline", int),
            "cache_loaded": field(last, r"session cache (\d+) bytes loaded", int),
            "cache_writes": field(last, r"(\d+) writes \(", int),
            "cache_last_bytes": field(last, r"last (\d+) bytes", int),
        })
        tail = lines[max(i for i, l in enumerate(lines) if COUNTERS.match(l)):]
        out["pso_cs_failed"] = field(last, r"compute \d+ ok (\d+) failed", int)
        for l in tail:
            m = re.search(r"PSO calls Xbox slot 57 \S+: (\d+) ok (\d+) failed", l)
            if m:
                out["slot57_ok"], out["slot57_failed"] = int(m.group(1)), int(m.group(2))
            m = re.search(r"(\d+) failed PSO calls, (\d+) refusal reasons", l)
            if m:
                out["failed_calls"], out["reasons"] = int(m.group(1)), int(m.group(2))
            m = re.search(r"cache feedback: (\d+) hits \(mean (\d+) ms\), (\d+) misses \(mean (\d+) ms\)", l)
            if m:
                out.update({"hits": int(m.group(1)), "hit_ms": int(m.group(2)), "misses": int(m.group(3)),
                            "miss_ms": int(m.group(4))})
        out["mesh_refused"] = sum(int(m.group(1)) for m in (re.search(r"vk-counters: (\d+) PSOs refused: mesh-shader", l)
                                                             for l in tail) if m)
        for mark in (30, 60):
            at = [r for r in reports if r[0] <= mark]
            if at:
                out[f"vk_cs@{mark}"] = field(at[-1][1], r"compute (\d+) built", int)
    out["fatal"] = any("FATAL" in l or "fatal signal" in l for l in lines)
    out["rc"] = next((l.split("rc=")[1].split()[0] for l in reversed(lines) if l.startswith("run.sh: rc=")), None)
    return out


if __name__ == "__main__":
    for p in sys.argv[1:]:
        print(" ".join(f"{k}={v}" for k, v in summary(p).items()))
