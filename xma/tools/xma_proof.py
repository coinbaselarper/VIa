"""Decode the extracted XMA2 sample with the engine and compare it with ffmpeg (offline reference).

    python3 -I xma_proof.py STREAMS_DIR OUT_DIR FFMPEG XMA_DECODE_X86_64 XMA_DECODE_ARM64

Per stream (serially, each binary with a timeout): ffmpeg decode, the engine from the start (x86_64 under Rosetta,
and arm64; outputs must be identical), the engine started at a third of the stream (frame-accurate seek), and the
engine looping the middle half twice. Writes OUT_DIR/proof.json and prints a summary. Decoded audio stays in OUT_DIR.
"""
import hashlib
import json
import os
import subprocess
import sys


def run(cmd, timeout=180):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if p.returncode != 0:
        raise RuntimeError("%s: rc %d %s" % (cmd[0], p.returncode, p.stderr[-400:]))
    return p.stdout


def sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def main():
    sdir, odir, ffmpeg, dec_x86, dec_arm = sys.argv[1:6]
    os.makedirs(odir, exist_ok=True)
    streams = json.load(open(os.path.join(sdir, "streams.json")))
    results = []
    for s in streams:
        name, rate, ch = s["name"], str(s["rate"]), str(s["channels"])
        xma = os.path.join(sdir, name + ".xma")
        ref = os.path.join(odir, name + ".ref.s16")
        r = {"name": name, "kind": s["kind"], "rate": s["rate"], "channels": s["channels"], "frames": s["frames"],
             "declared_samples": s["samples"], "loop_word": s["loop_word"]}
        try:
            run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", os.path.join(sdir, name + ".xma.wav"),
                 "-f", "s16le", "-acodec", "pcm_s16le", ref])
            ox, oa = os.path.join(odir, name + ".x86_64.s16"), os.path.join(odir, name + ".arm64.s16")
            r["full"] = json.loads(run([dec_x86, xma, rate, ch, "--ref", ref, "--out", ox]))
            r["full_arm64"] = json.loads(run([dec_arm, xma, rate, ch, "--ref", ref, "--out", oa]))
            r["arch_identical"] = sha(ox) == sha(oa)
            os.remove(oa)
            lag = str(r["full"]["lag"])
            total = s["frames"] * 512
            start = total // 3 + 37
            r["seek"] = json.loads(run([dec_x86, xma, rate, ch, "--ref", ref, "--start", str(start), "--lag", lag]))
            if s["frames"] >= 8:
                ls, le = total // 4 + 300, (3 * total) // 4 + 100
                r["loop"] = json.loads(run([dec_x86, xma, rate, ch, "--ref", ref, "--loop", str(ls), str(le),
                                            "--iterations", "2", "--lag", lag]))
        except Exception as e:  # noqa: BLE001
            r["error"] = str(e)
        results.append(r)
        print("%s %s full %.1f dB seek %.1f dB loop %s" % (
            name, s["kind"][:8], r.get("full", {}).get("psnr_db", -1), r.get("seek", {}).get("psnr_steady_db", -1),
            "%.1f/%.1f dB" % (r["loop"]["psnr_all_db"], r["loop"]["psnr_steady_db"]) if "loop" in r else "-"),
            flush=True)
    json.dump(results, open(os.path.join(odir, "proof.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
