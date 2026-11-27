import argparse, datetime, json, os, re, shutil, subprocess, sys, time
import xml.etree.ElementTree as ET

PORT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
BUILD = os.path.join(PORT, "build")
JOBS = 2

def now():
    return datetime.datetime.now().astimezone().isoformat(timespec="seconds")

def run(cmd, log, timeout, env=None):
    t0 = time.time()
    with open(log, "ab") as f:
        f.write(("$ " + " ".join(cmd) + "\n").encode())
        f.flush()
        p = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT, start_new_session=True, env=env)
        try:
            rc = p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(p.pid, 9)
            except ProcessLookupError:
                pass
            p.wait()
            rc = 124
    return rc, round(time.time() - t0, 1)

def discover_units():
    units = [{"name": "top", "src": PORT}]
    for dp, dn, fn in os.walk(PORT):
        rel = os.path.relpath(dp, PORT)
        if rel == "build" or rel.startswith("build" + os.sep) or "third_party" in rel.split(os.sep) or rel.count(os.sep) > 1:
            dn[:] = []
            continue
        if "CMakeLists.txt" in fn and rel != ".":
            try:
                txt = open(os.path.join(dp, "CMakeLists.txt"), errors="replace").read()
            except OSError:
                continue
            if re.search(r"^\s*project\s*\(", txt, re.M | re.I):
                units.append({"name": rel.replace(os.sep, "_"), "src": dp})
    return units

def tree(unit, arch):
    return os.path.join(BUILD, "ci-%s" % arch if unit["name"] == "top" else "ci-%s-%s" % (unit["name"], arch))

def module_of(d, unit, out):
    parts = out.split("/")
    if parts[0] == "port" and len(parts) > 2:
        parts = parts[1:]
    if len(parts) == 1:
        q = subprocess.run(["ninja", "-C", d, "-t", "query", out], capture_output=True, text=True).stdout
        ins = [l.strip() for l in q.split("input:")[-1].splitlines()[1:] if "/" in l]
        if ins:
            parts = ins[0].split("/")
            if parts[0] == "port" and len(parts) > 2:
                parts = parts[1:]
    first = parts[0]
    if first in ("CMakeFiles", "") or len(parts) == 1:
        return unit["name"] if unit["name"] != "top" else "top"
    return first if unit["name"] == "top" or first in ("shims_gdk", "d3d12_vulkan", "os", "isa", "seh", "loader", "shims_core", "host", "media", "portable", "compat") else unit["name"]


def configure_build(unit, arch, logdir, build_timeout):
    d = tree(unit, arch)
    rec = {"unit": unit["name"], "arch": arch, "tree": d, "configure_rc": None, "build_rc": None,
           "failed_modules": [], "excluded_modules": [], "start": now()}
    shutil.rmtree(d, ignore_errors=True)
    log = os.path.join(logdir, "%s-%s-build.log" % (unit["name"], arch))
    open(log, "w").close()
    base = ["cmake", "-S", unit["src"], "-B", d, "-G", "Ninja", "-DCMAKE_OSX_ARCHITECTURES=" + arch,
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo"]
    excluded = []
    allmods = ["os", "isa", "seh", "loader", "shims_core", "shims_gdk", "d3d12_vulkan", "host"]
    for _ in range(5):
        extra = []
        if excluded:
            extra = ["-DGTA6RT_MODULES=" + ";".join(m for m in allmods if m not in excluded)]
        rc, secs = run(base + extra, log, 600)
        rec["configure_rc"] = rc
        if rc == 0:
            break
        txt = open(log, errors="replace").read()
        m = re.findall(r"CMake Error at (\w+)/CMakeLists\.txt", txt)
        cand = [x for x in m if x in allmods and x not in excluded]
        if unit["name"] != "top" or not cand:
            break
        excluded.append(cand[-1])
        shutil.rmtree(d, ignore_errors=True)
    rec["excluded_modules"] = excluded
    if rec["configure_rc"] != 0:
        rec["end"] = now()
        return rec
    rc, secs = run(["ninja", "-C", d, "-k", "0", "-j", str(JOBS)], log, build_timeout)
    rec["build_rc"], rec["build_seconds"] = rc, secs
    txt = open(log, errors="replace").read()
    mods = set()
    for f in re.findall(r"^FAILED: (?:\[code=\d+\] )?(\S+)", txt, re.M):
        mods.add(module_of(d, unit, f))
    if rc != 0 and not mods:
        mods.add(unit["name"] if unit["name"] != "top" else "top")
    rec["failed_modules"] = sorted(mods)
    rec["end"] = now()
    return rec

def test_roots(d):
    found = set()
    for dp, dn, fn in os.walk(d):
        if "CMakeFiles" in dn:
            dn.remove("CMakeFiles")
        if "CTestTestfile.cmake" in fn:
            found.add(dp)
    roots = []
    for x in sorted(found):
        p = os.path.dirname(x)
        anc = False
        while p.startswith(d) and len(p) >= len(d):
            if p in found:
                anc = True
                break
            if p == d:
                break
            p = os.path.dirname(p)
        if not anc:
            roots.append(x)
    return roots

def run_tests(unit, arch, build_rec, logdir, test_timeout):
    d = tree(unit, arch)
    out = {"tests": [], "ctest_dirs": [], "no_tests": False}
    if build_rec["configure_rc"] != 0 or not os.path.isdir(d):
        return out
    roots = test_roots(d)
    jobs = "2" if arch == "arm64" else "1"
    for r in roots:
        tag = os.path.relpath(r, d).replace(os.sep, "_")
        junit = os.path.join(logdir, "%s-%s-%s.junit.xml" % (unit["name"], arch, tag))
        log = os.path.join(logdir, "%s-%s-%s-ctest.log" % (unit["name"], arch, tag))
        open(log, "w").close()
        if os.path.exists(junit):
            os.remove(junit)
        rc, secs = run(["ctest", "--test-dir", r, "--timeout", str(test_timeout), "-j", jobs,
                        "--output-junit", junit, "--output-on-failure"], log, 6 * 3600)
        stamp = now()
        out["ctest_dirs"].append({"dir": r, "rc": rc, "seconds": secs, "end": stamp, "log": log})
        if not os.path.exists(junit):
            continue
        rel = os.path.relpath(r, d)
        first = rel.split(os.sep)[0] if rel != "." else "top"
        module = first if unit["name"] == "top" else unit["name"]
        for tc in ET.parse(junit).getroot().iter("testcase"):
            status = "pass"
            detail = ""
            fail = tc.find("failure")
            if tc.get("status") == "notrun" or tc.find("skipped") is not None:
                status = "not_run"
            if fail is not None:
                status = "fail"
                detail = (fail.get("message") or "")[:200]
            if status != "pass":
                so = tc.find("system-out")
                if so is not None and so.text:
                    detail = (detail + " | " + so.text.strip()[-300:]).strip(" |")
                if "Could not find executable" in detail or "Unable to find executable" in detail:
                    status = "build_failed" if module in build_rec["failed_modules"] or build_rec["build_rc"] else "not_run"
            if "Timeout" in detail and status == "fail":
                detail = "TIMEOUT " + detail
            out["tests"].append({"unit": unit["name"], "arch": arch, "module": module, "name": tc.get("name"),
                                 "status": status, "seconds": float(tc.get("time") or 0), "timestamp": stamp,
                                 "detail": detail})
    if not out["tests"]:
        out["no_tests"] = True
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arch", default="both", choices=["arm64", "x86_64", "both"])
    ap.add_argument("--units")
    ap.add_argument("--test-timeout", type=int, default=120)
    ap.add_argument("--build-timeout", type=int, default=5400)
    ap.add_argument("--no-build", action="store_true", help="reuse existing ci trees")
    ap.add_argument("--build-only", action="store_true")
    ap.add_argument("--out")
    a = ap.parse_args()
    try:
        os.nice(10)
    except OSError:
        pass
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    outdir = a.out or os.path.join(BUILD, "ci-results", stamp)
    os.makedirs(outdir, exist_ok=True)
    archs = ["arm64", "x86_64"] if a.arch == "both" else [a.arch]
    units = discover_units()
    if a.units:
        want = a.units.split(",")
        units = [u for u in units if u["name"] in want]
    summary = {"started": now(), "machine": os.uname().machine, "units": [u["name"] for u in units],
               "builds": [], "tests": [], "ctest_dirs": [], "notes": []}
    free = shutil.disk_usage("/System/Volumes/Data").free / 2**30
    summary["disk_free_gib_start"] = round(free, 1)
    if free < 60:
        print("ci: refusing to run, %.1f GiB free < 60" % free)
        sys.exit(2)
    brecs = {}
    for arch in archs:
        for u in units:
            if a.no_build:
                brecs[(u["name"], arch)] = {"unit": u["name"], "arch": arch, "tree": tree(u, arch),
                                            "configure_rc": 0 if os.path.isdir(tree(u, arch)) else 1,
                                            "build_rc": 0, "failed_modules": [], "excluded_modules": []}
            else:
                print("[%s] build %s %s" % (now(), u["name"], arch), flush=True)
                brecs[(u["name"], arch)] = configure_build(u, arch, outdir, a.build_timeout)
            summary["builds"].append(brecs[(u["name"], arch)])
    if not a.build_only:
        for arch in archs:
            for u in units:
                print("[%s] test %s %s" % (now(), u["name"], arch), flush=True)
                t = run_tests(u, arch, brecs[(u["name"], arch)], outdir, a.test_timeout)
                summary["tests"] += t["tests"]
                summary["ctest_dirs"] += t["ctest_dirs"]
                if t["no_tests"] or not t["ctest_dirs"]:
                    summary["notes"].append("%s/%s: no CTest tests discovered" % (u["name"], arch))
    summary["finished"] = now()
    summary["disk_free_gib_end"] = round(shutil.disk_usage("/System/Volumes/Data").free / 2**30, 1)

    rows = {}
    for t in summary["tests"]:
        r = rows.setdefault((t["module"], t["arch"]), {"pass": 0, "fail": 0, "not_run": 0, "build_failed": 0})
        r[t["status"]] += 1
    for b in summary["builds"]:
        mods = ["%s(configure)" % b["unit"]] if b["configure_rc"] != 0 else b["failed_modules"]
        for m in mods:
            rows.setdefault((m, b["arch"]), {"pass": 0, "fail": 0, "not_run": 0, "build_failed": 0})
            rows[(m, b["arch"])]["build_failed"] += 1
    failed_build = [(b["unit"], b["arch"]) for b in summary["builds"] if b["configure_rc"] != 0 or b["build_rc"] not in (0, None)]
    ok = not failed_build and all(t["status"] == "pass" for t in summary["tests"])
    summary["ok"] = ok
    summary["totals"] = {s: sum(1 for t in summary["tests"] if t["status"] == s) for s in ("pass", "fail", "not_run", "build_failed")}
    with open(os.path.join(outdir, "summary.json"), "w") as f:
        json.dump(summary, f, indent=1)
    lines = ["%-16s %-7s %5s %5s %7s %7s  %s" % ("module", "arch", "pass", "fail", "notrun", "bldfail", "build")]
    bstat = {}
    for b in summary["builds"]:
        if b["configure_rc"] != 0:
            s = "CONFIGURE-FAIL"
        elif b["build_rc"]:
            s = "BUILD-FAIL(%s)" % ",".join(b["failed_modules"])
        else:
            s = "ok"
        if b["excluded_modules"]:
            s += " excluded=" + ",".join(b["excluded_modules"])
        bstat[(b["unit"], b["arch"])] = s
    for (m, arch), r in sorted(rows.items()):
        lines.append("%-16s %-7s %5d %5d %7d %7d" % (m, arch, r["pass"], r["fail"], r["not_run"], r["build_failed"]))
    lines.append("")
    for (u, arch), s in sorted(bstat.items()):
        lines.append("build %-14s %-7s %s" % (u, arch, s))
    for t in summary["tests"]:
        if t["status"] != "pass":
            lines.append("%s %-12s %s/%s %s %s" % (t["timestamp"], t["status"].upper(), t["module"], t["arch"], t["name"], t["detail"][:120].replace("\n", " ")))
    lines += summary["notes"]
    lines.append("totals %s ok=%s summary=%s" % (summary["totals"], ok, os.path.join(outdir, "summary.json")))
    txt = "\n".join(lines)
    open(os.path.join(outdir, "table.txt"), "w").write(txt + "\n")
    print(txt)
    sys.exit(0 if ok else 1)

main()
