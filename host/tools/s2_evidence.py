#!/usr/bin/env python3
"""Appends S2 evidence to a title run log from its present-time PNG dumps.

  s2_evidence.py LOG PRESENT_DIR START_EPOCH SCREEN_TEXT_BIN

For every dumped frame (present_NNNNNN.png, written at present time): seconds since START_EPOCH (file mtime), mean
colour and distinct colours (screen_text --stats). Frames with 65536+ colours are video. Runs of identical still frames
are pages; the first frame of each page is read with macOS Vision (screen_text). Summary lines start with "[s2] ".
"""
import os
import re
import subprocess
import sys

FIRST_RUN_PAGES = ("NAVIGATION EXPERIENCE", "UI TEXT SIZE", "BRIGHTNESS", "FINISH SETUP")
LANDING = "ACCOUNT PICKER"


def main():
    log, present, start, tool = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
    frames = sorted(f for f in os.listdir(present) if re.fullmatch(r"present_\d+\.png", f))
    out = []
    if not frames:
        out.append("[s2] no present dumps in %s" % present)
        return write(log, out)
    paths = [os.path.join(present, f) for f in frames]
    res = subprocess.run([tool, "--stats"] + paths, capture_output=True, text=True)
    rows = []
    for line in res.stdout.splitlines():
        path, mean, colours = line.split("\t")
        n = int(re.search(r"present_(\d+)", path).group(1))
        c = colours.split()[1]
        rows.append({"n": n, "path": path, "t": int(os.stat(path).st_mtime) - start, "mean": mean,
                     "colours": 65536 if c.endswith("+") else int(c), "video": c.endswith("+")})
    for r in rows:
        out.append("[s2] frame %d +%ds %s colours %s%s" % (r["n"], r["t"], r["mean"], r["colours"],
                                                         "+ (video)" if r["video"] else ""))
    lit = [r for r in rows if r["colours"] > 1]
    if lit:
        out.append("[s2] first lit dump: frame %d at +%ds" % (lit[0]["n"], lit[0]["t"]))
    def rgb(r):
        return [float(v) for v in r["mean"].split()[1:]]

    def same(a, b):
        return (max(abs(x - y) for x, y in zip(rgb(a), rgb(b))) <= 1.0 and
                abs(a["colours"] - b["colours"]) <= 0.05 * max(a["colours"], b["colours"]))

    pages, prev = [], None
    for r in rows:
        if r["video"] or r["colours"] <= 1:
            prev = None
            continue
        if prev is None or not same(prev, r):
            pages.append([r])
        else:
            pages[-1].append(r)
        prev = r
    pages = [p for p in pages if len(p) >= 2]
    texts = {}
    if pages:
        res = subprocess.run([tool] + [p[0]["path"] for p in pages], capture_output=True, text=True)
        for line in res.stdout.splitlines():
            path, _, text = line.partition("\t")
            texts[path] = text
    landing = None
    first_run_pages = []
    for p in pages:
        r = p[0]
        text = texts.get(r["path"], "")
        out.append("[s2] page from frame %d (+%ds, %d dumps): %s" % (r["n"], r["t"], len(p), text or "(no text)"))
        up = text.upper()
        if LANDING in up and landing is None:
            landing = r
        for title in FIRST_RUN_PAGES:
            if title in up and title not in [t for t, _ in first_run_pages]:
                first_run_pages.append((title, r))
                break
    video = [r for r in rows if r["video"]]
    if video:
        out.append("[s2] launch movie: %d dumped frames with video content (65536+ colours), frames %d-%d, +%ds to +%ds"
                   % (len(video), video[0]["n"], video[-1]["n"], video[0]["t"], video[-1]["t"]))
    else:
        out.append("[s2] launch movie: none of the dumped frames has video content")
    if landing:
        out.append("[s2] landing prompt (ACCOUNT PICKER / PLAYER (A)) on screen from frame %d, +%ds: %s"
                   % (landing["n"], landing["t"], landing["path"]))
    for title, r in first_run_pages:
        out.append("[s2] first-run page reached: %s (frame %d, +%ds): %s" % (title, r["n"], r["t"], r["path"]))
    if not first_run_pages:
        out.append("[s2] no first-run page found in the dumps")
    write(log, out)


def write(log, lines):
    with open(log, "a", encoding="utf-8", newline="\n") as f:
        for line in lines:
            f.write(line + "\n")
    for line in lines:
        if not line.startswith("[s2] frame "):
            print(line)


if __name__ == "__main__":
    main()
