"""Build the real-shim A/B programs from a built shims_gdk tree, without touching port/shims_gdk or that tree.

usage: python3 -I scripts/build_shim_ab.py ARCH [VARIANT[:WINDOW_MIB] ...]

ARCH is arm64 or x86_64. GDK_BUILD (default port/build/dstorage-ARCH) must be a built tree of shims_gdk with dstorage_test built.
Everything is written to port/build/dstorage-ab-ARCH:
  a/      baseline/: the backend as it was before the io backend patch was applied (one worker thread per queue, 64 MiB window, leonida inflate), recompiled
  b/      the live port/shims_gdk/src backend (pool, F_NOCACHE, system zlib, 256 MiB window)
  bench_a, bench_b, bench_b_fd     tests/dstorage_replay_bench.cpp against a, b, b + the FileNativeFd test row
  stress_a, stress_b, stress_b_fd  shims_gdk/tests/dstorage_pool_stress_test.cpp, same three
  test_a, test_b, test_b_fd        the existing dstorage_test (489 checks), same three
  VARIANT[:WINDOW_MIB]             extra live builds with StagingPool::limit set (bench_<VARIANT>_fd), e.g. b64:64
VARIANT names are [A-Za-z0-9_]+.
"""
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MOD = os.path.dirname(HERE)
PORT = os.path.dirname(MOD)
arch = sys.argv[1]
if arch not in ("arm64", "x86_64"):
    sys.exit("ARCH is arm64 or x86_64")
gdk_build = os.environ.get("GDK_BUILD", f"{PORT}/build/dstorage-{arch}")
out = f"{PORT}/build/dstorage-ab-{arch}"
os.makedirs(out, exist_ok=True)
ccs = json.load(open(f"{gdk_build}/compile_commands.json"))
tmpl = next(e for e in ccs if e["file"].endswith("shims_gdk/src/dstorage.cpp"))
test_tmpl = next(e for e in ccs if e["file"].endswith("shims_gdk/tests/dstorage_test.cpp"))


def run(cmd, cwd):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, cwd=cwd)
    if r.returncode:
        sys.exit(f"failed: {cmd[:200]}\n{r.stderr[:3000]}")


def sources(name, window=None):
    d = f"{out}/{name}"
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d + "/src")
    if name == "a":
        for f in ("dstorage.cpp", "dstorage_staging.h"):
            shutil.copy(f"{MOD}/baseline/{f}", f"{d}/src/{f}")
    else:
        for f in os.listdir(f"{PORT}/shims_gdk/src"):
            if f.startswith("dstorage"):
                shutil.copy(f"{PORT}/shims_gdk/src/{f}", f"{d}/src/{f}")
        if window:
            p = f"{d}/src/dstorage_staging.h"
            t = open(p).read().replace("limit = 256ull << 20;", f"limit = {window}ull << 20;")
            open(p, "w").write(t)
    return d


def compile_to(src, obj, base=tmpl, prelude=False):
    i = base["command"].index(" -o ")
    cmd = base["command"][:i] + f" -o {obj} -c {src}"
    run(cmd, base["directory"])


def lib_with(obj, name):
    lib = f"{out}/lib_{name}.a"
    shutil.copy(f"{gdk_build}/shims_gdk/libgta6rt_shims_gdk.a", lib)
    run(f"ar d {lib} dstorage.cpp.o", out)
    run(f"ar r {lib} {obj} 2>/dev/null; ranlib {lib} 2>/dev/null", out)
    return lib


def link(exe, objs, lib, extra=""):
    cmds = subprocess.run(["ninja", "-C", gdk_build, "-t", "commands", "shims_gdk/dstorage_test"], capture_output=True, text=True).stdout.strip().splitlines()[-1]
    cmd = cmds.strip(": &").rstrip("&: ").strip()
    cmd = cmd.replace("-o shims_gdk/dstorage_test", f"-o {out}/{exe}")
    cmd = cmd.replace("shims_gdk/CMakeFiles/dstorage_test.dir/tests/dstorage_test.cpp.o", objs)
    cmd = cmd.replace(f"-Wl,-force_load,{gdk_build}/shims_gdk/libgta6rt_shims_gdk.a", f"-Wl,-force_load,{lib}")
    cmd = cmd.replace("  shims_gdk/libgta6rt_shims_gdk.a ", f"  {lib} ")
    run(cmd + f" {extra} -lz", gdk_build)


variants = [("a", None), ("b", None)]
for spec in sys.argv[2:]:
    name, _, win = spec.partition(":")
    if not re.fullmatch(r"[A-Za-z0-9_]+", name) or name in ("a", "b") or (win and not win.isdigit()):
        sys.exit(f"bad variant {spec!r}: NAME[:WINDOW_MIB], NAME in [A-Za-z0-9_]+ and not a or b")
    variants.append((name, int(win) if win else None))

compile_to(f"{PORT}/shims_gdk/tests/dstorage_native_fd_row.cpp", f"{out}/native_fd_row.o", test_tmpl)
for name, base in (("test", None), ("stress", f"{PORT}/shims_gdk/tests/dstorage_pool_stress_test.cpp"), ("bench", f"{MOD}/tests/dstorage_replay_bench.cpp")):
    if base:
        compile_to(base, f"{out}/{name}.o", test_tmpl)
for name, window in variants:
    d = sources(name, window)
    obj = f"{out}/{name}/dstorage.cpp.o"
    compile_to(f"{d}/src/dstorage.cpp", obj)
    lib = lib_with(obj, name)
    fd_row = f"-Wl,-force_load,{out}/native_fd_row.o"
    if name in ("a", "b"):
        for kind, o in (("test", f"{gdk_build}/shims_gdk/CMakeFiles/dstorage_test.dir/tests/dstorage_test.cpp.o"), ("stress", f"{out}/stress.o"), ("bench", f"{out}/bench.o")):
            link(f"{kind}_{name}", o, lib)
            if name == "b":
                link(f"{kind}_b_fd", o, lib, fd_row)
    else:
        link(f"bench_{name}_fd", f"{out}/bench.o", lib, fd_row)
print("built", out)
