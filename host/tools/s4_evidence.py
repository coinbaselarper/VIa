#!/usr/bin/env python3
"""Evidence lines for an S4 Metal world run (host/tools/s4_metal_world.sh), printed as "[s4] ..." on stdout.

  s4_evidence.py LOG META WORLD_DUMP_DIR [SHAPES_TSV]

Times are seconds since the process started (the log's t=, the input script's clock and the dump files' mtimes
against the launcher's start epoch agree to about a second).
- world: the "[host] stage t=...: world" line.
- still view / control: macOS Vision text of the world presents (build/macos/tools/screen_text): the loading tips
  ("Tip"/"gunfire"/...) before control, the HUD's money ("$407") once the game camera has it.
- lit presents: d3d12_vulkan/tools/png_stats.py on up to 16 presents spread over the world phase from world+20 s:
  a present is lit when >= 90% of its pixels are not black, its mean RGB is 16..240 and it has >= 2000 colours;
  "pass" needs >= 8 sampled and >= 75% of them lit (the fade to the game camera is a few black presents).
- camera turns: for each scripted rsright, the mean absolute grey difference (0-255, 240 px wide copies) between the
  last present before it and the presents during it, against the difference between the two presents before it;
  "changed" when during >= 3 x before + 8.
- end and GPU errors: the launcher's meta (watchdog / wired / no world), run.sh's rc, the backend's exits (3 GPU error,
  4 footprint cap, 5 command buffer over the abort time), [metal] command-buffer failures / FAULTED lines,
  fault_stop.sh, and the gpuEvent-gta6rt report count before and after.
"""
import os
import re
import statistics
import subprocess
import sys
import tempfile

PORT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
PNG_STATS = os.path.join(PORT, "d3d12_vulkan", "tools", "png_stats.py")
SCREEN_TEXT = os.path.join(PORT, "build", "macos", "tools", "screen_text")
TIP = re.compile(r"\bTip\b|gunfire|loading|Loading", re.I)
MONEY = re.compile(r"\$\s?[0-9][0-9,]{1,}")


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read().splitlines()
    except OSError:
        return []


def grey_small(path, work, width=240):
    out = os.path.join(work, os.path.basename(path)[:-4] + ".bmp")
    if not os.path.exists(out):
        subprocess.run(["sips", "-s", "format", "bmp", "--resampleWidth", str(width), path, "--out", out],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    import struct
    data = open(out, "rb").read()
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    step = struct.unpack_from("<H", data, 28)[0] // 8
    stride = (w * step + 3) & ~3
    flip, h = h > 0, abs(h)
    rows = []
    for y in range(h):
        base = off + (h - 1 - y if flip else y) * stride
        row = data[base:base + w * step]
        rows.append([(row[x + 2] * 299 + row[x + 1] * 587 + row[x] * 114) // 1000 for x in range(0, w * step, step)])
    return rows


def mad(a, b):
    total = n = 0
    for ra, rb in zip(a, b):
        for x, y in zip(ra, rb):
            total += abs(x - y)
            n += 1
    return total / n if n else 0.0


def main():
    log, meta_path, dumps = sys.argv[1], sys.argv[2], sys.argv[3]
    lines, meta = read(log), read(meta_path)
    start = None
    for m in meta:
        g = re.search(r"^attempt \d+ start \S+ epoch (\d+)", m)
        if g:
            start = int(g.group(1))
    world = None
    fps, hud, foot = [], [], []
    for l in lines:
        g = re.match(r"\[host\] stage t=([\d.]+)s: world", l)
        if g and world is None:
            world = float(g.group(1))
        g = re.match(r"\[rt\] submit phases t=(\d+)s: .*?([\d.]+) frames/s", l)
        if g and world is not None and int(g.group(1)) > world + 10:
            fps.append(float(g.group(2)))
        g = re.search(r"footprint (\d+) MiB", l)
        if g and l.startswith(("[gov] memory", "[I os] memory")):
            foot.append(int(g.group(1)))
        if l.startswith("[rt] hud (no window): ") and world is not None:
            hud.append(l)
    print("[s4] world phase reached at +%.1fs" % world if world is not None else "[s4] world phase not reached")

    pngs = []
    if os.path.isdir(dumps):
        for n in sorted(os.listdir(dumps)):
            if n.endswith(".png"):
                p = os.path.join(dumps, n)
                pngs.append((os.path.getmtime(p) - start if start else 0.0, p))
    print("[s4] world presents dumped: %d" % len(pngs))

    first_hud = last_tip = None
    if pngs and os.access(SCREEN_TEXT, os.X_OK):
        out = subprocess.run([SCREEN_TEXT] + [p for _, p in pngs], capture_output=True, text=True).stdout
        text = {}
        for row in out.splitlines():
            if "\t" in row:
                k, v = row.split("\t", 1)
                text[k] = v
        for t, p in pngs:
            s = text.get(p, "")
            if TIP.search(s) and first_hud is None:
                last_tip = t
            if MONEY.search(s) and first_hud is None:
                first_hud = t
    if world is not None:
        print("[s4] still view with tips seen until %s; HUD (control) first seen at %s" % (
            "world%+.0fs" % (last_tip - world) if last_tip is not None else "-",
            "world%+.0fs" % (first_hud - world) if first_hud is not None else "never"))

    lit_ok = lit_n = 0
    sample = [x for x in pngs if world is not None and x[0] >= world + 20]
    if len(sample) > 16:
        sample = [sample[i * (len(sample) - 1) // 15] for i in range(16)]
    worst = ""
    for t, p in sample:
        r = subprocess.run([sys.executable, "-I", PNG_STATS, p], capture_output=True, text=True).stdout
        g = re.search(r"lit \d+/\d+ \(([\d.]+)%\) mean rgb ([\d.]+) ([\d.]+) ([\d.]+) colours (\d+)", r)
        if not g:
            continue
        lit_n += 1
        pct, mean, colours = float(g.group(1)), sum(float(g.group(i)) for i in (2, 3, 4)) / 3, int(g.group(5))
        if pct >= 90 and 16 <= mean <= 240 and colours >= 2000:
            lit_ok += 1
        elif not worst:
            worst = " (first unlit: %s at world%+.0fs: lit %.1f%%, mean %.1f, colours %d)" % (
                os.path.basename(p), t - world, pct, mean, colours)
    verdict = "pass" if lit_n >= 8 and lit_ok * 4 >= lit_n * 3 else "fail"
    print("[s4] lit world presents: %d of %d sampled from world+20s (png_stats: lit >= 90%%, mean RGB 16..240, colours >= 2000; "
          "pass: >= 8 sampled, >= 75%% lit) -> %s%s" % (lit_ok, lit_n, verdict, worst))

    turns = []
    for l in lines:
        g = re.match(r"\[rt\] gameinput: scripted (rsright|rsleft)@([\d.]+) held from ([\d.]+) s for ([\d.]+) s", l)
        if g:
            turns.append((float(g.group(3)), float(g.group(4))))
    work = tempfile.mkdtemp(prefix="s4-")
    changed = 0
    for t0, hold in turns:
        before = [p for t, p in pngs if t0 - 8 <= t < t0]
        during = [p for t, p in pngs if t0 + 0.5 <= t <= t0 + hold + 2]
        if world is None or t0 < world or len(before) < 2 or not during:
            continue
        a, b = grey_small(before[-2], work), grey_small(before[-1], work)
        base = mad(a, b)
        best = max(mad(b, grey_small(p, work)) for p in during)
        ok = best >= 3 * base + 8
        changed += ok
        print("[s4] camera turn at world%+.0fs: %s (difference during %.1f, before %.1f)" % (
            t0 - world, "changed the frame" if ok else "no change", best, base))
    print("[s4] camera turns that changed the frame: %d of %d" % (changed, len(turns)))

    rc = None
    ends = []
    for m in meta:
        g = re.match(r"attempt \d+ rc (\d+)", m)
        if g:
            rc = int(g.group(1))
        if m.startswith("end ") and "gpuEvent" not in m:
            ends.append(m[4:])
        if m.startswith("[fault_stop]"):
            ends.append("GPU error: " + m)
    ev = [m for m in meta if "gpuEvent after" in m]
    ev_before = ev_after = None
    if ev:
        g = re.search(r"gpuEvent after (\d+) \(before (\d+)\)", ev[-1])
        if g:
            ev_after, ev_before = int(g.group(1)), int(g.group(2))
    code = {3: "GPU error (backend exit 3)", 4: "footprint cap (exit 4)", 5: "command buffer over the abort time (exit 5)"}
    if rc in code:
        ends.append(code[rc])
    killed = [l for l in lines if l.startswith("run.sh: killed")]
    if killed:
        ends.append("run.sh timer")
    end_t = None
    for l in lines:
        g = re.match(r"\[rt\] submit phases t=(\d+)s", l)
        if g:
            end_t = int(g.group(1))
    stayed = (end_t - world) if world is not None and end_t is not None else 0
    print("[s4] end: %s; rc %s; last periodic line at world%+.0fs" % ("; ".join(ends) if ends else "process exited", rc, stayed))
    errs = [l for l in lines if re.match(r"^\[metal\].*(command buffer [0-9]+ failed|FAULTED|kIOGPUCommandBufferCallbackError)", l)]
    new_ev = (ev_after - ev_before) if ev_after is not None else -1
    print("[s4] GPU errors: %d (command-buffer failures / faults in the log %d, new gpuEvent-gta6rt reports %s, exit %s)" % (
        len(errs) + max(new_ev, 0) + (1 if rc in (3, 5) else 0), len(errs), new_ev if new_ev >= 0 else "?", rc))
    watchdog = any(e.startswith("watchdog world+") for e in ends)
    if watchdog and not errs and new_ev == 0 and rc not in (3, 4, 5):
        g = re.search(r"world\+(\d+)", next(e for e in ends if e.startswith("watchdog")))
        print("[s4] world for %s s: ended by the watchdog" % g.group(1))
    if fps:
        print("[s4] frames/s in the world (from world+10s): median %.1f, min %.1f, max %.1f" % (
            statistics.median(fps), min(fps), max(fps)))
    wired = [int(x.split()[1]) for x in read(os.path.join(os.path.dirname(log), os.path.basename(log)[:-4] + ".wired.tsv")) if x.strip()]
    print("[s4] memory: footprint peak %s MiB, machine wired peak %s MiB" % (max(foot) if foot else "?", max(wired) if wired else "?"))


if __name__ == "__main__":
    main()
