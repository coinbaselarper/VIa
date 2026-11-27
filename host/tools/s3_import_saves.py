#!/usr/bin/env python3
"""Imports a VIctory save set into the layout the port's offline XGameSave shim serves.

  s3_import_saves.py SRC [--saves DIR] [--storage DIR] [--dry-run] [--force]

SRC is a folder holding VIctory's data directory for the title (any parent of `saves/<XUID>/<SCID>/...`), e.g.
saves/manual-save-2026-11-26/extracted/Manual Save/victory/data/7952987A.

VIctory layout (VIctory/crates/victory-hle/src/modules/xgamesave.rs): saves/<XUID %016X>/<configId>/<name>~<fnv64>/vN/
with `manifest` (u32-length-prefixed strings: container name, display name, then each blob name) and blob i in file
`i`; the highest vN is the container. Persistent local storage is storage/<path> (storage.rs).

Port layout (shims_gdk/src/xgameruntime_gamesave.cpp): <saves>/local-user/<enc(container)>/<enc(blob)> with the
display name in `.display`; persistent local storage is <overlay>/.gta6rt-persistent/<path>. Names are %-encoded:
[A-Za-z0-9_-] and '.' (not first) stay, every other UTF-8 byte becomes %XX.

The source is only read. Every written file is compared back to its source (SHA-256). A destination file that
exists with other bytes is left alone and reported unless --force. Writes import-manifest.json beside local-user.
"""
import argparse
import hashlib
import json
import os
import struct
import sys

PORT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def encode(name):
    out = []
    for c in name.encode("utf-8"):
        ch = chr(c)
        if (ch.isascii() and ch.isalnum()) or ch in "_-" or (ch == "." and out):
            out.append(ch)
        else:
            out.append("%%%02X" % c)
    return "".join(out)


def parse_manifest(data):
    names, at = [], 0
    while at < len(data):
        if at + 4 > len(data):
            return None
        (n,) = struct.unpack_from("<I", data, at)
        at += 4
        if at + n > len(data):
            return None
        names.append(data[at:at + n].decode("utf-8"))
        at += n
    return names if len(names) >= 2 else None


def fnv1a64(s):
    h = 0xCBF29CE484222325
    for b in s.encode("utf-8"):
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def find_containers(src):
    for root, dirs, files in os.walk(src):
        dirs.sort()
        versions = [d for d in dirs if d.startswith("v") and d[1:].isdigit()]
        if not versions or "~" not in os.path.basename(root):
            continue
        best = max(versions, key=lambda d: int(d[1:]))
        vdir = os.path.join(root, best)
        mpath = os.path.join(vdir, "manifest")
        if not os.path.isfile(mpath):
            yield {"dir": root, "error": "no manifest in %s" % vdir}
            continue
        with open(mpath, "rb") as f:
            names = parse_manifest(f.read())
        if not names:
            yield {"dir": root, "error": "manifest does not parse: %s" % mpath}
            continue
        rel = os.path.relpath(root, src).split(os.sep)
        yield {"dir": root, "version": best, "container": names[0], "display": names[1], "blobs": names[2:],
               "xuid": rel[-3] if len(rel) >= 3 else None, "config": rel[-2] if len(rel) >= 2 else None,
               "hash_ok": os.path.basename(root).endswith("~%016x" % fnv1a64(names[0])), "vdir": vdir}


def copy(src, dst, dry, force, report):
    want = sha256(src)
    if os.path.exists(dst):
        have = sha256(dst)
        if have == want:
            report.append(("same", dst))
            return want
        if not force:
            report.append(("differs, kept (use --force)", dst))
            return None
    report.append(("write", dst))
    if dry:
        return want
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    tmp = dst + "~import"
    with open(src, "rb") as f, open(tmp, "wb") as g:
        g.write(f.read())
    st = os.stat(src)
    os.utime(tmp, (st.st_atime, st.st_mtime))
    os.replace(tmp, dst)
    if sha256(dst) != want:
        raise SystemExit("read-back mismatch: %s" % dst)
    return want


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("--saves", default=os.path.join(PORT, "run", "saves"), help="LEONIDA_SAVE_DIR (default port/run/saves)")
    ap.add_argument("--storage", default=os.path.join(PORT, "run", "overlay", ".gta6rt-persistent"),
                    help="persistent local storage folder (default port/run/overlay/.gta6rt-persistent)")
    ap.add_argument("--no-storage", action="store_true", help="import the save containers only")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args()
    src = os.path.abspath(a.src)
    if not os.path.isdir(src):
        raise SystemExit("%s: not a folder" % src)
    entries, report, failed = [], [], 0
    for c in find_containers(src):
        if "error" in c:
            print("[import] %s: %s" % (c["dir"], c["error"]))
            failed += 1
            continue
        cdir = os.path.join(a.saves, "local-user", encode(c["container"]))
        blobs = []
        for i, blob in enumerate(c["blobs"]):
            s = os.path.join(c["vdir"], str(i))
            if not os.path.isfile(s):
                print("[import] %s: blob %d (%s) missing" % (c["vdir"], i, blob))
                failed += 1
                continue
            d = os.path.join(cdir, encode(blob))
            h = copy(s, d, a.dry_run, a.force, report)
            if h is None:
                failed += 1
            with open(s, "rb") as f:
                head = f.read(48)
            blobs.append({"blob": blob, "file": d, "source": s, "size": os.path.getsize(s), "sha256": h,
                          "magic": head[:4].decode("latin-1") if head[:4] == b"ASAV" else head[:4].hex(), "asav_version": struct.unpack_from("<I", head, 4)[0]
                          if head[:4] == b"ASAV" else None, "asav_kind": struct.unpack_from("<I", head, 8)[0]
                          if head[:4] == b"ASAV" else None})
        disp = os.path.join(cdir, ".display")
        report.append(("display", disp))
        if not a.dry_run:
            os.makedirs(cdir, exist_ok=True)
            with open(disp, "w", encoding="utf-8", newline="") as f:
                f.write(c["display"])
        entries.append({"container": c["container"], "display": c["display"], "version": c["version"],
                        "xuid": c["xuid"], "config": c["config"], "folder_hash_matches_name": c["hash_ok"],
                        "source": c["dir"], "blobs": blobs})
        print("[import] container %s (display \"%s\", %s, xuid %s, config %s): %s" % (
            c["container"], c["display"], c["version"], c["xuid"], c["config"],
            ", ".join("\"%s\" %d bytes%s" % (b["blob"], b["size"], " ASAV v%s kind %s" % (b["asav_version"], b["asav_kind"])
                                                    if b["magic"] == "ASAV" else "") for b in blobs)))
    storage = []
    sroot = None
    for root, dirs, files in os.walk(src):
        if os.path.basename(root) == "storage" and root != src:
            sroot = root
            break
    if sroot and not a.no_storage:
        for root, dirs, files in os.walk(sroot):
            dirs.sort()
            for name in sorted(files):
                s = os.path.join(root, name)
                rel = os.path.relpath(s, sroot)
                d = os.path.join(a.storage, rel)
                h = copy(s, d, a.dry_run, a.force, report)
                if h is None:
                    failed += 1
                storage.append({"path": rel, "file": d, "source": s, "size": os.path.getsize(s), "sha256": h})
                print("[import] storage %s: %d bytes" % (rel, os.path.getsize(s)))
            for dname in dirs:
                p = os.path.join(a.storage, os.path.relpath(os.path.join(root, dname), sroot))
                if not a.dry_run:
                    os.makedirs(p, exist_ok=True)
    for what, path in report:
        if what != "same":
            print("[import] %s %s" % (what, path))
    if not a.dry_run:
        os.makedirs(a.saves, exist_ok=True)
        with open(os.path.join(a.saves, "import-manifest.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump({"source": src, "containers": entries, "storage": storage}, f, indent=1)
    print("[import] %d container(s), %d storage file(s), %d problem(s)%s" % (
        len(entries), len(storage), failed, " (dry run)" if a.dry_run else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
