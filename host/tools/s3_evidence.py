#!/usr/bin/env python3
"""Appends S3 evidence to a title run log: stages, pages, memory, streaming, pipelines, threads.

  s3_evidence.py LOG PRESENT_DIR START_EPOCH SCREEN_TEXT_BIN [DSTORAGE_SHAPES_TSV]

Reads only lines the host and the layers print:
  [host] stage t=<s>s: <stage> (threads N)             host/src/title_watch.cpp, on every change
  [rt] peek t=<s>s: ... threads=N max=M                 every --peek-seconds
  [rt] gameinput: press <button>@<s>                    scripted presses
  [I os] memory: periodic: resident R MiB, footprint F MiB, compressed C MiB, ...
  [rt] vk-counters: ...                                 pipeline counters (last values)
  [rt] xgamesave: ...                                   save calls
Pages: runs of identical still frames among the present dumps (as s2_evidence.py), read with macOS Vision. Every
summary line starts with "[s3] ".
"""
import collections
import os
import re
import subprocess
import sys

STAGE = re.compile(r"^\[host\] stage t=([0-9.]+)s: (.*) \(threads (\d+)\)")
PEEK = re.compile(r"^\[rt\] peek t=(\d+)s:.*threads=(\d+) max=(\d+)")
PRESS = re.compile(r"^\[rt\] gameinput: press (\w+)@([0-9.]+)")
MEM = re.compile(r"^\[I os\] memory: (periodic|[^:]+): resident (\d+) MiB, footprint (\d+) MiB, compressed (\d+) MiB")
SAVE = re.compile(r"^\[rt\] xgamesave: (.*)")
REGISTRY = re.compile(r"^\[rt\] d3d12: title textures \(refresh\):.* registry (\d+) images")
SUBMIT = re.compile(r"^\[rt\] submit phases t=(\d+)s: (\d+) submissions in ([0-9.]+) s .*?, ([0-9.]+) frames/s")
SLOWPSO = re.compile(r"^\[rt\] d3d12: (compute|graphics) pipeline .* took ([0-9.]+) s to build")
GUARD = re.compile(r"^\[host\] vk_guard: refused .*: (\d+) such, (\d+) views refused in all")
GIB24 = 24 * 1024


def frames_and_pages(present, start, tool):
    frames = sorted(f for f in os.listdir(present) if re.fullmatch(r"present_\d+\.png", f)) if os.path.isdir(present) else []
    if not frames:
        return [], []
    paths = [os.path.join(present, f) for f in frames]
    res = subprocess.run([tool, "--stats"] + paths, capture_output=True, text=True)
    rows = []
    for line in res.stdout.splitlines():
        path, mean, colours = line.split("\t")
        c = colours.split()[1]
        rows.append({"n": int(re.search(r"present_(\d+)", path).group(1)), "path": path,
                     "t": int(os.stat(path).st_mtime) - start, "mean": [float(v) for v in mean.split()[1:]],
                     "colours": 65536 if c.endswith("+") else int(c), "video": c.endswith("+")})

    still = [r for r in rows if r["colours"] > 1 and not r["video"]]
    if still:
        res = subprocess.run([tool] + [r["path"] for r in still], capture_output=True, text=True)
        texts = {}
        for line in res.stdout.splitlines():
            path, _, text = line.partition("\t")
            texts[path] = text
        for r in still:
            r["text"] = texts.get(r["path"], "")

    def key(r):
        if r["video"]:
            return ("video",)
        title = r.get("text", "").split(" | ")[0].strip()
        return ("text", title) if title else ("stats", round(r["mean"][0]), round(r["mean"][1]), round(r["mean"][2]))

    pages, prev = [], None
    for r in rows:
        if r["colours"] <= 1:
            prev = None
            continue
        k = key(r)
        if prev is None or k != prev:
            pages.append([r])
        else:
            pages[-1].append(r)
        prev = k
    return rows, pages


def main():
    log, present, start, tool = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
    shapes = sys.argv[5] if len(sys.argv) > 5 else None
    stages, peeks, presses, mems, saves, counters = [], [], [], [], [], collections.OrderedDict()
    registry, submits, slow, guard = [], [], collections.Counter(), 0
    last_t = 0.0
    with open(log, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = STAGE.match(line)
            if m:
                last_t = float(m.group(1))
                stages.append((last_t, m.group(2), int(m.group(3))))
                continue
            m = PEEK.match(line)
            if m:
                last_t = float(m.group(1))
                peeks.append((last_t, int(m.group(2)), int(m.group(3))))
                continue
            m = PRESS.match(line)
            if m:
                last_t = float(m.group(2))
                presses.append((last_t, m.group(1)))
                continue
            m = MEM.match(line)
            if m:
                mems.append((last_t, m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4))))
                continue
            m = SAVE.match(line)
            if m:
                saves.append(m.group(1))
                continue
            m = REGISTRY.match(line)
            if m:
                registry.append((last_t, int(m.group(1))))
                continue
            m = SUBMIT.match(line)
            if m:
                last_t = float(m.group(1))
                submits.append((last_t, float(m.group(4))))
                continue
            m = SLOWPSO.match(line)
            if m:
                slow[int(last_t) // 30 * 30] += 1
                continue
            m = GUARD.match(line)
            if m:
                guard = max(guard, int(m.group(2)))
                continue
            if line.startswith("[rt] vk-counters: "):
                body = line[len("[rt] vk-counters: "):].strip()
                key = re.sub(r"\d+", "N", body)
                counters[key] = body
    out = []
    for t, name, threads in stages:
        out.append("[s3] stage +%.1fs: %s (threads %d)" % (t, name, threads))
    names = [s[1] for s in stages]
    story = next((s for s in stages if s[1].startswith("story load")), None)
    world = next((s for s in stages if s[1].startswith("world")), None)
    if story:
        out.append("[s3] story load reached at +%.1fs (session 0, threads %d)" % (story[0], story[2]))
    else:
        out.append("[s3] story load not reached; last stage: %s" % (names[-1] if names else "none"))
    if world:
        out.append("[s3] world phase reached at +%.1fs: loading byte went 2 -> 0 in session 0 (threads %d)" % (world[0], world[2]))
    if story and world:
        out.append("[s3] story load took %.1f s" % (world[0] - story[0]))
    if peeks:
        out.append("[s3] threads: %d at the last peek (+%ds), at most %d" % (peeks[-1][1], peeks[-1][0], max(p[2] for p in peeks)))
    for s in saves:
        if "CreateContainer" in s or "ReadBlobData" in s or "GetContainerInfo" in s or "EnumerateContainerInfo" in s \
                or "SubmitBlobWrite" in s or "SubmitUpdate" in s:
            out.append("[s3] save call: " + s)
    if mems:
        peak = max(mems, key=lambda m: m[3])
        for t, why, res, foot, comp in mems:
            out.append("[s3] memory +%ds (%s): footprint %d MiB, resident %d MiB, compressed %d MiB" % (t, why, foot, res, comp))
        out.append("[s3] memory peak footprint %d MiB at +%ds (%.0f%% of 24 GiB); Series S title budget 7.6 GiB"
                   % (peak[3], peak[0], 100.0 * peak[3] / GIB24))
    if registry:
        out.append("[s3] streamed images registered (layer registry): %d at +%ds, %d at the last report (+%ds)"
                   % (registry[0][1], registry[0][0], registry[-1][1], registry[-1][0]))
        for mark in (1000, 10000, 30000, 38000):
            hit = next((r for r in registry if r[1] >= mark), None)
            if hit:
                out.append("[s3] registry passed %d images at +%ds" % (mark, hit[0]))
    if submits:
        out.append("[s3] presented frames/s over time (+s:fps): " + ", ".join("+%d:%.1f" % x for x in submits[::max(1, len(submits) // 30)]))
    if slow:
        out.append("[s3] pipelines over 2 s to build, per 30 s (+s:n): " + ", ".join("+%d:%d" % kv for kv in sorted(slow.items())))
    if guard:
        out.append("[s3] vk_guard: %d image views refused (Metal would abort on them)" % guard)
    for body in counters.values():
        out.append("[s3] pipelines (last): " + body)
    rows, pages = frames_and_pages(present, start, tool)
    lit = [r for r in rows if r["colours"] > 1]
    if lit:
        out.append("[s3] %d dumps, first lit at frame %d (+%ds), last frame %d (+%ds)" % (len(rows), lit[0]["n"], lit[0]["t"],
                                                                                       rows[-1]["n"], rows[-1]["t"]))
    seen = set()
    for p in pages:
        r = p[0]
        text = r.get("text", "")
        kind = "video" if r["video"] else "page"
        out.append("[s3] %s from frame %d (+%ds, %d dumps, %d colours): %s | %s"
                   % (kind, r["n"], r["t"], len(p), r["colours"], text or "(no text)", r["path"]))
        title = text.split(" | ")[0].strip() if text else ""
        if title and title not in seen and not r["video"]:
            seen.add(title)
    if shapes and os.path.isfile(shapes):
        out += dstorage_summary(shapes, start)
    with open(log, "a", encoding="utf-8", newline="\n") as f:
        for line in out:
            f.write(line + "\n")
    for line in out:
        print(line)


def dstorage_summary(path, start):
    files, by_file, by_codec, done, first, last = {}, collections.Counter(), collections.Counter(), collections.Counter(), None, None
    bytes_src, bytes_dst, n, failed = collections.Counter(), collections.Counter(), 0, 0
    per_min = collections.Counter()
    origin = 0
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("# ds-shapes"):
                m = re.search(r"unix=(\d+)", line)
                origin = int(m.group(1)) - start if m else 0
                continue
            col = line.rstrip("\n").split("\t")
            if col[0] == "F" and len(col) >= 3:
                files[col[1]] = col[2]
            elif col[0] == "R" and len(col) >= 12:
                t = int(col[1])
                first = t if first is None else first
                last = t
                fid, src, dst, opt = col[7], int(col[9]), int(col[11]), int(col[5], 16)
                name = files.get(fid, fid)
                name = re.sub(r"^.*[\\/]", "", name) if "\\" in name or "/" in name else name
                by_file[name] += 1
                bytes_src[name] += src
                bytes_dst[name] += dst
                codec = ("zlib+" if opt & 1 else "") + ("bc%d" % ((opt >> 1) & 0xF) if (opt >> 1) & 0xF else "raw")
                by_codec[codec] += 1
                per_min[(origin + t // 1000000) // 30 * 30] += src
                n += 1
            elif col[0] == "C" and len(col) >= 5:
                done[col[4]] += 1
                if col[4] not in ("0x00000000",):
                    failed += 1
    out = ["[s3] dstorage: %d requests, %.1f MiB read, %.1f MiB written, over %.0f s; %d failed"
           % (n, sum(bytes_src.values()) / 2**20, sum(bytes_dst.values()) / 2**20,
              ((last or 0) - (first or 0)) / 1e6, failed)]
    for name, count in by_file.most_common(12):
        out.append("[s3] dstorage file %s: %d requests, %.1f MiB read, %.1f MiB out" % (name, count, bytes_src[name] / 2**20,
                                                                                    bytes_dst[name] / 2**20))
    out.append("[s3] dstorage codecs: " + ", ".join("%s %d" % kv for kv in by_codec.most_common()))
    out.append("[s3] dstorage MiB read per 30 s (+s:MiB): " + ", ".join("+%d:%.0f" % (m, b / 2**20) for m, b in sorted(per_min.items())))
    bad = {k: v for k, v in done.items() if k != "0x00000000"}
    if bad:
        out.append("[s3] dstorage results other than S_OK: " + ", ".join("%s %d" % kv for kv in sorted(bad.items())))
    return out


if __name__ == "__main__":
    main()
