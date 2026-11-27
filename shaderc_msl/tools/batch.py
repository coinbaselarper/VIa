#!/usr/bin/env python3
"""Resumable parallel batch driver for the shaderc_msl recipe (tools/shaderc_msl.py).

  batch.py --spirv-dir <corpus> --build port/build/shaderc_msl [--jobs 4] [--limit N] [--stages vs,ps]
           [--order first-scenes] [--max-seconds S] [--pso ms|all|none]

Order: first the shaders named by pipeline-compile-begin lines in the title runs' logs (port/run/logs, --runtime-logs;
first-seen order, log files by modification time), then UI (Scaleform V*/F* families), blit, movie (Bink), then the
shaders of the title's pipeline library in record order (graphics records, then compute), then everything else.
--extra-dirs adds the translated sub-corpora next to the main one (library/, exe/, fspx/, bvh/: each has its own
index.json); their shaders are named "<dir>/<name>", which is how the runtime logs name them.
Outputs are content-addressed in <build>/cas (key = SPIR-V + translator + recipe + tool versions + pre-pass
parameters), so a re-run skips everything already built for the same inputs. Each run appends rows to
<build>/runs/<run>/results.jsonl and writes summary.json; manifest.json maps every shader name to its key, stage,
entry and binding plan for the backend.
Mesh shaders get their dead-output and component-trim lists from <build>/mesh/mesh_fit.jsonl (tools/mesh_fit.py).
"""
import argparse, collections, concurrent.futures as cf, json, os, re, resource, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shaderc_msl as S  # noqa: E402

S52_MEAN_S = {"vs": 0.069, "ps": 0.84, "cs": 6.5, "ms": 0.82}
STALL_S = 1200
UI_RE = re.compile(r"^.?(V|F|CF)[A-Z][A-Za-z0-9]*_[0-9a-f]{8}_Wrapped")


def base(stem):
    return stem[1:] if stem and not stem[0].isalpha() else stem


def classify(stem):
    b = base(stem)
    if "Bink" in b or "Movie" in b:
        return 2
    if "Blit" in b:
        return 1
    if UI_RE.match(stem):
        return 0
    return None


def runtime_order(pattern, index):
    import glob
    seq, seen = [], set()
    for f in sorted(glob.glob(pattern), key=os.path.getmtime):
        for line in open(f, errors="replace"):
            if "pipeline-compile-begin" not in line:
                continue
            m = re.search(r"shaders=(\S+)", line)
            for n in m.group(1).split(",") if m else []:
                if n in index and n not in seen:
                    seen.add(n)
                    seq.append(n)
    return seq


def load_index(spirv_dir, extra):
    index = json.load(open(os.path.join(spirv_dir, "index.json")))
    for d in [x for x in extra.split(",") if x]:
        sub = json.load(open(os.path.join(spirv_dir, d, "index.json")))
        for k, v in sub.items():
            index[d + "/" + k] = dict(v, corpus=d)
    return index


def order(index, pairs, runtime=()):
    first = collections.defaultdict(list)
    for k in sorted(index):
        c = classify(k)
        if c is not None:
            first[c].append(k)
    seq = list(runtime)
    seen = set(seq)
    for k in first[0] + first[1] + first[2]:
        if k not in seen:
            seen.add(k)
            seq.append(k)
    alias = pairs.get("digest_aliases", {}) if pairs else {}
    if pairs:
        for g in pairs["graphics"]:
            for s in ("vs", "ms", "ps"):
                n = g.get(s)
                for x in ([n] + alias.get(n, [])) if n else []:
                    if x in index and x not in seen:
                        seen.add(x)
                        seq.append(x)
        for c in pairs["compute"]:
            n = c.get("cs")
            for x in ([n] + alias.get(n, [])) if n else []:
                if x in index and x not in seen:
                    seen.add(x)
                    seq.append(x)
    seq += [k for k in sorted(index) if k not in seen]
    return seq, {"runtime_logs": len(runtime), "ui": len(first[0]), "blit": len(first[1]), "movie": len(first[2])}


def mesh_opts(path):
    out = {}
    if not os.path.exists(path):
        return out
    for line in open(path):
        r = json.loads(line)
        if r["step"] in (1, 2, 3) and r.get("drop") is not None:
            o = {"drop_outputs": r["drop"]}
            if r["step"] >= 2 and r.get("widths"):
                o["trim_outputs"] = r["widths"]
            out[r["stem"]] = o
    return out


def shader_opts(index, k, mo, dxbc_dir, vs_route="lane"):
    """Per-shader recipe options: mesh dead-output/trim lists, and the LDS shrink for compute shaders over 32 KiB
    whose RSRC2 allocation plus the wave64 exchange buffer fits."""
    st = index[k]["stage"]
    o = {"vs_route": vs_route}
    if st == "ms" and k in mo:
        o.update(mo[k])
    if st == "cs" and index[k].get("lds_bytes", 0) > 32768 and "/" not in k:
        alloc = S.lds_allocation(os.path.join(dxbc_dir, k + ".dxbc"))
        lds_array = 4 * 8192
        if alloc and alloc + index[k]["lds_bytes"] - lds_array <= 32768:
            o["shrink_lds"] = alloc // 4
        else:
            o["x_buf_device"] = True
    return o


_W = {}


def init_worker(spirv_dir, cas, translator, tmp_root):
    _W.update(spirv_dir=spirv_dir, cas=cas, translator=translator, tmp=os.path.join(tmp_root, str(os.getpid())))
    os.makedirs(_W["tmp"], exist_ok=True)


def work(item):
    stem, stage, opts = item
    t0 = time.monotonic()
    try:
        r = S.compile_one(os.path.join(_W["spirv_dir"], stem + ".spv"), stage, _W["cas"], _W["translator"], opts,
                          tmp=_W["tmp"])
    except Exception as e:  # noqa: BLE001
        r = {"status": "driver_error", "error": repr(e)[:400], "key": None}
    r = dict(r)
    r["stem"] = stem
    r["wall_s"] = round(time.monotonic() - t0, 3)
    r["peak_child_rss_mb"] = round(S.PEAK["rss"] / 2**20, 1)
    return r


def bucket(r):
    e = r.get("error", "")
    if r["status"] == "ok":
        return "ok"
    e = re.sub(r"program_source:\d+:\d+: ", "", e)
    e = re.sub(r"/[^ ]*\.metal:\d+:\d+: ", "", e)
    e = re.sub(r"\(\d+\)", "(N)", e)
    e = re.sub(r"'[^']*'", "'X'", e)
    return r["status"] + ": " + e.strip()[:140]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--spirv-dir", required=True)
    ap.add_argument("--build", required=True)
    ap.add_argument("--run", default="main")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--stages", default="")
    ap.add_argument("--max-seconds", type=float, default=0)
    ap.add_argument("--vs-route", default="lane")
    ap.add_argument("--summary-only", action="store_true")
    ap.add_argument("--extra-dirs", default="library,exe,fspx,bvh")
    ap.add_argument("--redo", default="", help="comma-separated stem substrings: compute only these, even if this "
                    "run has them (the content-addressed store still skips unchanged keys)")
    ap.add_argument("--runtime-logs", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "run",
                                                           "logs", "*.log"))
    ap.add_argument("--names-file", default="", help="only these shader names, one per line, in file order")
    ap.add_argument("--jobs-file", default="", help="file holding the number of jobs to run now (1..--jobs), "
                    "re-read after every shader")
    ap.add_argument("--huge-mb", type=float, default=6.0, help="SPIR-V size from which a shader runs alone in its "
                    "class: at most one such shader is in flight (the other slots take normal shaders)")
    ap.add_argument("--fast-resume", action="store_true", help="skip shaders already in this run's results by name "
                    "(default: recompute each key and let the content-addressed cache skip unchanged ones)")
    a = ap.parse_args()
    jobs = max(1, min(a.jobs, 3))
    index = load_index(a.spirv_dir, a.extra_dirs)
    pairs_p = os.path.join(a.build, "pipelines", "pairs.json")
    pairs = json.load(open(pairs_p)) if os.path.exists(pairs_p) else None
    seq, firsts = order(index, pairs, runtime_order(a.runtime_logs, index))
    if a.summary_only:
        run_dir = os.path.join(a.build, "runs", a.run)
        done = {}
        for line in open(os.path.join(run_dir, "results.jsonl")):
            r = json.loads(line)
            done[r["stem"]] = r
        summarize(a, index, done, run_dir, 0, 0, 0, a.jobs)
        return
    if a.stages:
        keep = set(a.stages.split(","))
        seq = [k for k in seq if index[k]["stage"] in keep]
    if a.names_file:
        want = [x.rstrip("\n") for x in open(a.names_file) if x.strip()]
        missing = [x for x in want if x not in index]
        if missing:
            sys.exit(f"--names-file: {len(missing)} names not in the corpus, first {missing[0]!r}")
        seq = want
    if a.limit:
        seq = seq[:a.limit]
    mo = mesh_opts(os.path.join(a.build, "mesh", "mesh_fit.jsonl"))
    run_dir = os.path.join(a.build, "runs", a.run)
    os.makedirs(run_dir, exist_ok=True)
    res_p = os.path.join(run_dir, "results.jsonl")
    done = {}
    if os.path.exists(res_p):
        for line in open(res_p):
            try:
                r = json.loads(line)
                done[r["stem"]] = r
            except (ValueError, KeyError):
                pass
    todo = []
    dxbc_dir = os.path.join(os.path.dirname(os.path.abspath(a.spirv_dir)), "dxbc")
    if a.redo:
        seq = [k for k in seq if any(x in k for x in a.redo.split(","))]
    for k in seq:
        if (a.fast_resume and k in done and done[k].get("status") not in ("driver_error", "killed")
                and not (a.redo and any(x in k for x in a.redo.split(",")))):
            continue
        st = index[k]["stage"]
        todo.append((k, st, shader_opts(index, k, mo, dxbc_dir, a.vs_route)))
    tr = S.translator_id(a.spirv_dir)
    S.tool_versions()
    cas = os.path.join(a.build, "cas")
    print(f"batch: {len(seq)} selected ({firsts}), {len(done)} already in this run, {len(todo)} to do, jobs {jobs}", flush=True)
    t0 = time.monotonic()
    n = new = cached = 0
    stop = False
    with open(res_p, "a") as fo, cf.ProcessPoolExecutor(jobs, initializer=init_worker,
                                                        initargs=(a.spirv_dir, cas, tr, os.path.join(a.build, "tmp"))) as ex:
        huge_b = int(a.huge_mb * 2**20)
        normal, huge = [], []
        for item in todo:
            (huge if os.path.getsize(os.path.join(a.spirv_dir, item[0] + ".spv")) >= huge_b else normal).append(item)
        normal.reverse()
        huge.reverse()
        pending = {}

        def allowed():
            if a.jobs_file:
                try:
                    return max(1, min(jobs, int(open(a.jobs_file).read().split()[0])))
                except (OSError, ValueError, IndexError):
                    pass
            return jobs

        def top_up():
            cap = allowed()
            while not stop and len(pending) < cap:
                running_huge = any(h for h in pending.values())
                if huge and not running_huge:
                    item, is_huge = huge.pop(), True
                elif normal:
                    item, is_huge = normal.pop(), False
                else:
                    return
                pending[ex.submit(work, item)] = is_huge

        top_up()
        while pending:
            fin, _ = cf.wait(set(pending), return_when=cf.FIRST_COMPLETED)
            for f in fin:
                del pending[f]
                r = f.result()
                r["batch_ts"] = round(time.time(), 1)
                fo.write(json.dumps(r) + "\n")
                fo.flush()
                done[r["stem"]] = r
                n += 1
                cached += 1 if r.get("cached") else 0
                new += 0 if r.get("cached") else 1
                if n % 200 == 0 or r["status"] not in ("ok",) and not r.get("cached"):
                    el = time.monotonic() - t0
                    print(f"{n}/{len(todo)} {el:.0f}s new {new} ({3600 * new / max(el, 1e-9):.0f}/h) {r['status']} {r['stem'][:60]} "
                          f"{r.get('error', '')[:100]}", flush=True)
            if a.max_seconds and time.monotonic() - t0 > a.max_seconds:
                stop = True
            top_up()
    wall = time.monotonic() - t0
    summarize(a, index, done, run_dir, wall, new, cached, jobs)


def discrete_of(plan):
    """Descriptor arrays SPIRV-Cross passes as plain buffers (runtime-sized arrays cannot live in a tier-1 argument
    buffer): set -> buffer index. Plans made before msl_plan recorded them carry the parameter text instead."""
    if plan.get("discrete") is not None:
        return {int(k): v for k, v in plan["discrete"].items()}
    out = {}
    for b in plan.get("other_buffers", []):
        m = re.search(r"\bspvDescriptorSet(\d+)Binding0\b", b["param"])
        if m:
            out[int(m.group(1))] = b["index"]
    return out


def step_s(r):
    """Tool time of one shader (pre-pass + SPIRV-Cross + metal + -O0 retry + metallib); excludes time the driver was
    suspended, which total_s and wall_s include."""
    return round(sum(r.get(k) or 0 for k in ("pre_s", "cross_s", "metal_s", "metal_o0_s", "metallib_s")), 3)


def summarize(a, index, done, run_dir, wall, new, cached, jobs):
    rows = [r for r in done.values() if r["stem"] in index]
    gk = lambda k: (index[k].get("corpus", "main"), index[k]["stage"])
    by = collections.defaultdict(collections.Counter)
    buckets = collections.Counter()
    times = collections.defaultdict(list)
    for r in rows:
        g = "%s:%s" % gk(r["stem"])
        by[g][r["status"]] += 1
        buckets[(g, bucket(r))] += 1
        if r.get("cross_s") is not None:
            times[g].append(step_s(r))
    tim = {}
    for g, v in times.items():
        v = sorted(v)
        tim[g] = {"n": len(v), "mean_s": round(sum(v) / len(v), 3), "median_s": v[len(v) // 2],
                  "p95_s": v[int(0.95 * (len(v) - 1))], "max_s": v[-1], "sum_s": round(sum(v), 1)}
    measured = {r["stem"]: step_s(r) for r in rows if r.get("cross_s") is not None}
    sizes = {}
    for k in index:
        try:
            sizes[k] = os.path.getsize(os.path.join(a.spirv_dir, k + ".spv"))
        except OSError:
            sizes[k] = 0
    per_byte = {}
    for st in {v["stage"] for v in index.values()}:
        sb = sum(sizes[k] for k in measured if index[k]["stage"] == st)
        ss = sum(t for k, t in measured.items() if index[k]["stage"] == st)
        per_byte[st] = ss / sb if sb else None
    proj, proj_src = 0.0, {}
    for k in index:
        g = "%s:%s" % gk(k)
        e = proj_src.setdefault(g, {"rows": 0, "measured_rows": 0, "measured_job_s": 0.0, "estimated_rows": 0,
                                    "estimated_job_s": 0.0})
        e["rows"] += 1
        if k in measured:
            e["measured_rows"] += 1
            e["measured_job_s"] += measured[k]
        else:
            st = index[k]["stage"]
            pb = per_byte.get(st)
            est = sizes[k] * pb if pb else S52_MEAN_S.get(st, 0.1)
            e["estimated_rows"] += 1
            e["estimated_job_s"] += est
    for e in proj_src.values():
        e["measured_job_s"] = round(e["measured_job_s"], 1)
        e["estimated_job_s"] = round(e["estimated_job_s"], 1)
        proj += e["measured_job_s"] + e["estimated_job_s"]
    ts = sorted(r["batch_ts"] for r in rows if r.get("batch_ts") and not r.get("cached"))
    active = sum(min(b - a_, STALL_S) if b - a_ < STALL_S else 0 for a_, b in zip(ts, ts[1:]))
    stalls = [round(b - a_) for a_, b in zip(ts, ts[1:]) if b - a_ >= STALL_S]
    res = {"rows": len(rows), "by_stage": {k: dict(v) for k, v in sorted(by.items())},
           "buckets": {f"{g} | {b}": n for (g, b), n in sorted(buckets.items(), key=lambda x: (-x[1], x[0]))},
           "this_invocation": {"wall_s": round(wall, 1), "new": new, "cached": cached, "jobs": jobs,
                               "new_per_hour": round(3600 * new / wall, 1) if wall > 0 else None},
           "all_invocations": {"compiled_rows": len(ts), "active_wall_s": round(active, 1),
                               "excluded_stalls_s": stalls,
                               "shaders_per_hour": round(3600 * len(ts) / active, 1) if active else None},
           "compile_time_by_group": tim,
           "projection": {"job_seconds_all": round(proj, 0),
                          "wall_hours_at_4_jobs": round(proj / 4 / 3600, 2) if proj else None,
                          "by_group": proj_src,
                          "seconds_per_spirv_MB_by_stage": {k: round(v * 2**20, 3) for k, v in per_byte.items() if v},
                          "method": "measured step_s (tool time) of compiled rows + unbuilt rows x SPIR-V bytes x the "
                                    "stage's measured seconds per byte; / 4 jobs, ideal scaling"},
           "self_rusage_maxrss_mb": round(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 2**20, 1),
           "cas_bytes": cas_bytes(os.path.join(a.build, "cas"))}
    json.dump(res, open(os.path.join(run_dir, "summary.json"), "w"), indent=1)
    mesh_step = {}
    mf = os.path.join(a.build, "mesh", "mesh_fit.jsonl")
    if os.path.exists(mf):
        for line in open(mf):
            x = json.loads(line)
            mesh_step[x["stem"]] = x["step"]
    mp = os.path.join(a.build, "mesh", "mesh_pso.jsonl")
    if os.path.exists(mp):
        for line in open(mp):
            x = json.loads(line)
            if not x.get("split64") and x["pso"] != "OK" and "Total mesh size" in x.get("error", "") \
                    and "unique scalar" not in x.get("error", ""):
                mesh_step[x["stem"]] = 3
    route_of = {0: "hw", 1: "hw", 2: "hw", 3: "split64", 4: "mesh_as_compute"}
    mr_path = os.path.join(a.build, "mesh", "mesh_route.json")
    mesh_route = json.load(open(mr_path)) if os.path.exists(mr_path) else {}
    man = {}
    for r in rows:
        if r.get("key"):
            man[r["stem"]] = {"key": r["key"], "stage": index[r["stem"]]["stage"], "status": r["status"],
                              "metallib": f"cas/{r['key'][:2]}/{r['key']}.metallib" if r["status"] == "ok" else None,
                              "entry": (r.get("plan") or {}).get("entry"), "sets": (r.get("plan") or {}).get("sets"),
                              "push": (r.get("plan") or {}).get("push"), "passes": [p["pass"] for p in r.get("passes", [])], "passes_full": r.get("passes", []), "x_buf_buffer": xbuf_buffer(r.get("plan") or {}),
                              "descriptor_arrays": discrete_of(r.get("plan") or {}),
                              "mesh_route": (mesh_route.get(r["stem"], {}).get("route") or route_of.get(mesh_step.get(r["stem"]))) if index[r["stem"]]["stage"] == "ms" else None,
                              "mesh_info": mesh_route.get(r["stem"]) if index[r["stem"]]["stage"] == "ms" else None,
                              "vertex_as_kernel": "--msl-vertex-for-tessellation" in r.get("cross_extra", [])}
    json.dump(man, open(os.path.join(run_dir, "manifest.json"), "w"))
    with open(os.path.join(run_dir, "manifest.tsv"), "w") as f:
        f.write("# shaderc_msl manifest v3 root=../..\n")
        f.write("# name key stage entry metallib push tables(binding:buffer) vertex_as_kernel passes "
                "descriptor_arrays(binding:buffer) mesh_route(hw|split64|mesh_as_compute|unresolved) "
                "split_metallib split_object_metallib split_chunks split_vmax split_pmax split_push split_tables "
                "msc_cs_metallib msc_vs_metallib msc_cs_push msc_vs_push msc_cs_tables msc_vs_tables "
                "msc_layout(slot_bytes,vertices,vertex_stride,primitives,primitive_stride,max_vertices,max_primitives,"
                "address_dword,push_bytes,local_x) msc_params(loc:offset;...) cs_route(x_buf_device|-) x_buf_words\n")
        for name in sorted(man):
            m = man[name]
            if m["status"] != "ok" or "\t" in name or "\n" in name:
                continue
            sets = m.get("sets") or {}
            tables = ",".join(f"{int(s) - 1}:{i}" for s, i in sorted(sets.items(), key=lambda kv: int(kv[0])))
            f.write("\t".join([name, m["key"], m["stage"], m["entry"] or "-", m["metallib"],
                                str(m["push"]) if m["push"] is not None else "-", tables or "-",
                                "1" if m["vertex_as_kernel"] else "0", ",".join(m["passes"]) or "-",
                                ",".join(f"{int(k) - 1}:{v}" for k, v in sorted(m["descriptor_arrays"].items(), key=lambda kv: int(kv[0]))) or "-",
                                m["mesh_route"] or "-"] + mesh_cols(m.get("mesh_info")) + cs_cols(m)) + "\n")
    with open(os.path.join(run_dir, "unsupported.tsv"), "w") as f:
        f.write("# shaders with no manifest row: name, stage, status, reason (geometry shaders: no Metal stage; a PSO naming one "
                "is refused with this reason)\n")
        for r in sorted(rows, key=lambda r: r["stem"]):
            if r["status"] != "ok" and "\t" not in r["stem"]:
                why = "geometry shader: no Metal stage; in no pipeline-library record and no logged pipeline (STATUS.md section 4)" \
                    if index[r["stem"]]["stage"] == "gs" else (r.get("error") or r["status"])[:160]
                f.write("\t".join([r["stem"], index[r["stem"]]["stage"], r["status"], why.replace("\n", " ")]) + "\n")
    print(json.dumps({k: res[k] for k in ("rows", "by_stage", "this_invocation", "projection")}, indent=1))


def lib_path(key):
    return f"cas/{key[:2]}/{key}.metallib" if key else "-"


def tables_of(sets):
    return ",".join(f"{int(s) - 1}:{i}" for s, i in sorted((sets or {}).items(), key=lambda kv: int(kv[0]))) or "-"


def mesh_cols(info):
    """Manifest v3 columns after mesh_route: the split variant (object + mesh libraries) and the mesh-as-compute
    variant (compute kernel + vertex shader), each '-' when the route does not use it."""
    none = ["-"] * 15
    if not info:
        return none
    if info["route"] == "split64":
        p = info.get("plan") or {}
        sets = {k: v for k, v in (p.get("sets") or {}).items() if int(k) < 30}
        return [lib_path(info["key"]), lib_path(info["object_key"]), str(info["chunks"]), str(info["vmax"]), str(info["pmax"]),
                str(p["push"]) if p.get("push") is not None else "-", tables_of(sets)] + ["-"] * 8
    if info["route"] == "mesh_as_compute":
        l = info["layout"]
        lay = ",".join(str(x) for x in (l["slot_bytes"], l["vertices"], l["vertex_stride"], l["primitives"], l["primitive_stride"],
                                        l["max_vertices"], l["max_primitives"], l["address_dwords"][0], l["push_constant_bytes"],
                                        info["local_size"][0]))
        params = ";".join(f"{k}:{v}" for k, v in sorted(l["params"].items(), key=lambda kv: int(kv[0])))
        return ["-"] * 7 + [lib_path(info["cs_key"]), lib_path(info["vs_key"]), str(info["cs_push"]), str(info["vs_push"]),
                            tables_of(info["cs_sets"]), tables_of(info["vs_sets"]), lay, params or "-"]
    return none


def xbuf_buffer(plan):
    if plan.get("x_buf_buffer") is not None:
        return plan["x_buf_buffer"]
    sets = {int(k): v for k, v in (plan.get("sets") or {}).items()}
    if 30 in sets:
        return sets[30]
    ob = [b for b in plan.get("other_buffers", []) if b["param"].startswith("device ") and "spvDescriptor" not in b["param"]]
    return ob[0]["index"] if len(ob) == 1 else None


def cs_cols(m):
    xb = next((p for p in m.get("passes_full", []) if p.get("pass") == "x_buf_device"), None)
    return ["x_buf_device", str(xb["x_buf_words"]), str(m["x_buf_buffer"])] if xb else ["-", "-", "-"]


def cas_bytes(cas):
    tot = n = 0
    for d in os.listdir(cas) if os.path.isdir(cas) else []:
        p = os.path.join(cas, d)
        if os.path.isdir(p):
            for f in os.listdir(p):
                if f.endswith(".metallib"):
                    tot += os.path.getsize(os.path.join(p, f))
                    n += 1
    return {"metallibs": n, "bytes": tot}


if __name__ == "__main__":
    sys.exit(main())
