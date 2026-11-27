"""Extract a stratified sample of XMA2 streams for decoder checks (diagnosis only; keep the output local).

    python3 -I xma_extract.py CENSUS_DIR AUDIO_RPF OUT_DIR [--per-kind-granular N] [--per-kind-other N]

Reads CENSUS_DIR/xma_streams.csv (awc_census.py). For each chosen stream writes OUT_DIR/<bank>_<id>.xma (the raw
2048-byte packets of the 0x55 tag), <bank>_<id>.xma.wav (RIFF XMA2 for ffmpeg) and a line in OUT_DIR/streams.json.
Also writes OUT_DIR/packets.json: packet-header statistics over every XMA2 stream in the census.
"""
import csv
import json
import os
import struct
import sys
from collections import Counter


def riff_xma2(packets: bytes, rate: int, channels: int, samples: int) -> bytes:
    blocks = 1
    fmt = struct.pack("<HHIIHHH", 0x166, channels, rate, rate * channels * 2, 2048, 16, 34)
    fmt += struct.pack("<HIIIIIIIBBH", 1, 4 if channels == 1 else 3, samples, len(packets), 0, samples, 0, 0, 0, 4,
                       blocks)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(packets)) + packets
    return b"RIFF" + struct.pack("<I", len(body)) + body


def stratified(rows, n):
    rows = sorted(rows, key=lambda r: int(r["samples"]) / int(r["rate"]))
    if n >= len(rows):
        return rows
    step = len(rows) / n
    return [rows[int(i * step + step / 2)] for i in range(n)]


def main():
    census, rpf, out = sys.argv[1:4]
    n_gran = int(sys.argv[sys.argv.index("--per-kind-granular") + 1]) if "--per-kind-granular" in sys.argv else 140
    n_other = int(sys.argv[sys.argv.index("--per-kind-other") + 1]) if "--per-kind-other" in sys.argv else 100
    os.makedirs(out, exist_ok=True)
    rows = list(csv.DictReader(open(os.path.join(census, "xma_streams.csv"))))
    gran = [r for r in rows if r["kind"].startswith("granular")]
    other = [r for r in rows if not r["kind"].startswith("granular")]
    pick = {(r["bank_offset"], r["stream_id"]): r for r in stratified(gran, n_gran) + stratified(other, n_other)}
    for r in rows:
        if int(r["loop_word"]) != -1 or int(r["rate"]) != 32000:
            pick[(r["bank_offset"], r["stream_id"])] = r
    hdr = Counter()
    frames_hist = Counter()
    skip_hist = Counter()
    meta_hist = Counter()
    partial_tail = 0
    chosen = []
    with open(rpf, "rb") as f:
        for r in rows:
            f.seek(int(r["bank_offset"]) + int(r["data_offset"]))
            data = f.read(int(r["data_size"]))
            hdr["streams"] += 1
            hdr["bytes"] += len(data)
            if len(data) % 2048:
                hdr["streams_not_whole_packets"] += 1
            n = len(data) // 2048
            hdr["packets"] += n
            frames = 0
            for p in range(n):
                b = data[p * 2048:p * 2048 + 4]
                frames_hist[b[0] >> 2] += 1
                skip_hist[b[3]] += 1
                meta_hist[b[2] & 7] += 1
                frames += b[0] >> 2
            if frames * 512 < int(r["samples"]):
                partial_tail += 1
            hdr["frames"] += frames
            key = (r["bank_offset"], r["stream_id"])
            if key in pick:
                name = "%s_%s" % (r["bank_offset"], r["stream_id"])
                with open(os.path.join(out, name + ".xma"), "wb") as g:
                    g.write(data)
                with open(os.path.join(out, name + ".xma.wav"), "wb") as g:
                    g.write(riff_xma2(data, int(r["rate"]), int(r["channels"]), int(r["samples"])))
                chosen.append({"name": name, "kind": r["kind"], "rate": int(r["rate"]), "channels": int(r["channels"]),
                               "samples": int(r["samples"]), "loop_word": int(r["loop_word"]), "packets": n,
                               "frames": frames, "fmt_hex": r["fmt_hex"]})
    hdr["streams_with_fewer_frames_than_samples"] = partial_tail
    stats = {"totals": hdr, "frames_per_packet": dict(sorted(frames_hist.items())),
             "packet_skip": dict(sorted(skip_hist.items())), "metadata": dict(sorted(meta_hist.items()))}
    json.dump(stats, open(os.path.join(out, "packets.json"), "w"), indent=1)
    json.dump(chosen, open(os.path.join(out, "streams.json"), "w"), indent=1)
    print(json.dumps(stats))
    print("chosen", len(chosen), Counter(c["kind"] for c in chosen))


if __name__ == "__main__":
    main()
