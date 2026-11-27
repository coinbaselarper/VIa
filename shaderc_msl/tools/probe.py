"""Driver for bin/msl_probe (one long-lived process, JSON lines, restart on timeout) and stub generation."""
import json, os, re, select, subprocess, time

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_PROBE = os.path.join(HERE, "../../build/shaderc_msl/bin/msl_probe")


def build_probe(out=DEFAULT_PROBE):
    src = os.path.join(HERE, "msl_probe.mm")
    if os.path.exists(out) and os.path.getmtime(out) >= os.path.getmtime(src):
        return out
    os.makedirs(os.path.dirname(out), exist_ok=True)
    subprocess.run(["clang++", "-std=c++20", "-fobjc-arc", "-O2", src, "-framework", "Metal", "-framework", "Foundation",
                    "-o", out], check=True)
    return out


class Probe:
    def __init__(self, path=None):
        self.path = path or build_probe()
        self.p = None

    def _start(self):
        self.p = subprocess.Popen([self.path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1,
                                  start_new_session=True)

    def close(self):
        if self.p:
            try:
                self.p.kill()
                self.p.wait(5)
            except Exception:
                pass
            self.p = None

    def run(self, job, timeout=300):
        if self.p is None or self.p.poll() is not None:
            self._start()
        t0 = time.monotonic()
        self.p.stdin.write(json.dumps(job) + "\n")
        self.p.stdin.flush()
        r, _, _ = select.select([self.p.stdout], [], [], timeout)
        if not r:
            self.close()
            return {"result": "TIMEOUT", "error": f"no pipeline within {timeout} s", "pso_s": time.monotonic() - t0}
        line = self.p.stdout.readline()
        if not line:
            code = self.p.poll()
            self.close()
            return {"result": "CRASH", "error": f"probe exited {code}"}
        return json.loads(line)


COLOR_KIND = {"float": "f", "half": "f", "uint": "u", "ushort": "u", "int": "i", "short": "i"}


def color_string(colors):
    slots = ["-"] * 8
    for idx, t in colors:
        base = re.match(r"[a-z]+", t).group(0)
        if idx < 8:
            slots[idx] = COLOR_KIND.get(base, "f")
    while slots and slots[-1] == "-":
        slots.pop()
    return ",".join(slots)


def vs_stub_for(main0_in):
    members = []
    for line in (main0_in or "").splitlines():
        m = re.match(r"\s*(\w+) (\w+) \[\[user\((locn\d+(?:_\d+)?)\)[^\]]*\]\];", line)
        if m:
            members.append((m.group(1), m.group(2), m.group(3)))
    body = "\n".join(f"    {t} {n} [[user({l})]];" for t, n, l in members)
    return ("#include <metal_stdlib>\nusing namespace metal;\nstruct stub_out\n{\n    float4 pos [[position]];\n"
            + body + "\n};\nvertex stub_out stub_main(uint i [[vertex_id]])\n{\n    stub_out o = {};\n    o.pos = float4(0, 0, 0, 1);\n    return o;\n}\n")


def job_for(row, lib, stub_dir, reflect=True):
    plan = row.get("plan", {})
    entry = plan.get("entry")
    mode = {"vertex": "vs", "fragment": "ps", "kernel": "cs", "mesh": "ms"}.get(entry)
    if row.get("cross_extra") and "--msl-vertex-for-tessellation" in row["cross_extra"]:
        mode = "vs_kernel"
    job = {"key": row["key"], "mode": mode, "lib": lib, "fn": "main0", "reflect": reflect}
    if mode == "ps":
        f = plan.get("features", {})
        stub = os.path.join(stub_dir, row["key"] + ".stub.metal")
        open(stub, "w").write(vs_stub_for(f.get("main0_in")))
        job["stub"] = stub
        job["colors"] = color_string(f.get("colors", []))
        job["depth"] = bool(f.get("depth"))
        job["stencil"] = bool(f.get("stencil"))
    return job
