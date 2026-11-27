"""Census of the AWC (ADAT) audio banks in RPF9 archives, by codec, bytes and stream kind.

    python3 -I awc_census.py OUT_DIR ARCHIVE...

Banks are found by their raw little-endian ADAT header (the TOC is not read), with the header rules of
leonida/tools/rsc8_audio.py (parse_adat_header_bytes) and the format/music layouts of leonida/docs/audio.md.
Read-only on the archives. Writes OUT_DIR/census.json (totals) and OUT_DIR/xma_streams.csv (one row per
codec-5 stream with what is needed to extract it). No audio is written.
"""
import csv
import json
import mmap
import os
import struct
import sys
import time
from collections import defaultdict

MAX_HEADER_BYTES = 32 * 1024 * 1024
MAX_BANK_STREAMS = 200_000
CODEC = {0: "pcm16", 2: "pcm_f32", 5: "xma2", 11: "opus"}


def parse_bank(m, off, size):
    if off + 16 > size:
        return None
    flags, count, hdr = struct.unpack_from("<III", m, off + 4)
    if count > MAX_BANK_STREAMS or hdr < 16 or hdr > MAX_HEADER_BYTES or off + hdr > size:
        return None
    pos = 16
    if flags & 0x00400000:
        pos += 4
    if flags & 0x00010000:
        pos += count * 2
    if flags & 0x00200000:
        pos += (count + 1) * 4
    if count == 0:
        return (flags, [], off + hdr) if pos == hdr else None
    if pos + count * 4 > hdr:
        return None
    ids = struct.unpack_from("<%dI" % count, m, off + pos)
    pos += count * 4
    ntags = sum(w >> 29 for w in ids)
    if pos + ntags * 8 > hdr:
        return None
    raw = struct.unpack_from("<%dQ" % ntags, m, off + pos) if ntags else ()
    end = hdr
    streams, k = [], 0
    for w in ids:
        tags = {}
        for _ in range(w >> 29):
            q = raw[k]
            k += 1
            tt, tsz, toff = q >> 56, (q >> 28) & 0x0FFFFFFF, q & 0x0FFFFFFF
            if off + toff + tsz > size:
                return None
            end = max(end, toff + tsz)
            tags.setdefault(tt, (toff, tsz))
        streams.append((w & 0x1FFFFFFF, tags))
    return flags, streams, off + end


def music_channel_bytes(m, base, mus, data):
    (m_at, m_size), (d_at, d_size) = mus, data
    if m_size < 12:
        return None
    blocks, chunk, channels = struct.unpack_from("<III", m, base + m_at)
    if not (1 <= channels <= 32 and blocks >= 1 and chunk >= 2048 and m_size == 12 + 16 * channels):
        return None
    info = []
    for c in range(channels):
        b = base + m_at + 12 + 16 * c
        samples, = struct.unpack_from("<I", m, b + 4)
        rate, = struct.unpack_from("<H", m, b + 10)
        info.append([m[b + 12], rate, samples, 0])
    if (blocks - 1) * chunk >= d_size:
        return None
    for blk in range(blocks):
        at = base + d_at + blk * chunk
        bsz = min(chunk, d_size - blk * chunk)
        if channels * 24 > bsz:
            return None
        for c in range(channels):
            cnt, = struct.unpack_from("<I", m, at + 24 * c + 4)
            info[c][3] += cnt * 2048
    return info


def kind_of(archive, tags, codec):
    if 0x5C in tags:
        return "speech (lip-sync clip)"
    if 0x5A in tags:
        return "granular (0x5a grain table)"
    if 0xBD in tags:
        return "cue markers (0xbd)"
    if archive.startswith("story000") or archive.startswith("shared_minigame"):
        return "story/minigame other"
    return "other"


def main():
    out_dir, archives = sys.argv[1], sys.argv[2:]
    os.makedirs(out_dir, exist_ok=True)
    totals = defaultdict(lambda: [0, 0, 0.0])
    per_archive = {}
    xma_rows = []
    t0 = time.time()
    for path in archives:
        name = os.path.basename(path)
        size = os.path.getsize(path)
        a = {"size": size, "banks": 0, "bank_bytes": 0, "streams": 0, "by_codec": defaultdict(lambda: [0, 0, 0.0]),
             "by_kind": defaultdict(lambda: [0, 0, 0.0]), "music_banks": 0, "music_channels_by_codec": defaultdict(int),
             "xma_banks": 0, "xma_mixed_banks": 0, "rejected_magic": 0}
        with open(path, "rb") as f:
            m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            pos = 0
            while True:
                i = m.find(b"ADAT", pos)
                if i < 0:
                    break
                bank = parse_bank(m, i, size)
                if bank is None:
                    a["rejected_magic"] += 1
                    pos = i + 4
                    continue
                flags, streams, end = bank
                a["banks"] += 1
                a["bank_bytes"] += end - i
                bank_codecs = set()
                for sid, tags in streams:
                    a["streams"] += 1
                    data, fmt, mus = tags.get(0x55), tags.get(0x76), tags.get(0x48)
                    if mus and data:
                        a["music_banks"] += 1
                        chans = music_channel_bytes(m, i, mus, data) or []
                        for codec, rate, samples, nbytes in chans:
                            cname = CODEC.get(codec, "codec%d" % codec)
                            a["music_channels_by_codec"][cname] += 1
                            secs = samples / 48000.0
                            for key in (a["by_codec"][cname], a["by_kind"]["streamed music (0x48)"],
                                        totals[("all", cname, "streamed music (0x48)")]):
                                key[0] += 1
                                key[1] += nbytes
                                key[2] += secs
                            bank_codecs.add(codec)
                        continue
                    if not (data and fmt and fmt[1] >= 32):
                        continue
                    fo = i + fmt[0]
                    samples, loop = struct.unpack_from("<Ii", m, fo)
                    rate, = struct.unpack_from("<H", m, fo + 8)
                    codec, channels = m[fo + 28], m[fo + 29]
                    cname = CODEC.get(codec, "codec%d" % codec)
                    bank_codecs.add(codec)
                    if codec == 11:
                        secs = samples / max(channels, 1) / 48000.0
                    else:
                        secs = samples / rate if rate else 0.0
                    kind = kind_of(name, tags, codec)
                    for key in (a["by_codec"][cname], a["by_kind"][kind], totals[("all", cname, kind)]):
                        key[0] += 1
                        key[1] += data[1]
                        key[2] += secs
                    if codec == 5:
                        xma_rows.append({
                            "archive": name, "bank_offset": i, "bank_size": end - i, "bank_streams": len(streams),
                            "stream_id": "%08X" % sid, "channels": channels, "rate": rate, "samples": samples,
                            "loop_word": loop, "data_offset": data[0], "data_size": data[1],
                            "tags": " ".join("%02x" % t for t in sorted(tags)), "kind": kind,
                            "fmt_hex": m[fo:fo + fmt[1]].hex(),
                        })
                if 5 in bank_codecs:
                    a["xma_banks"] += 1
                    if len(bank_codecs) > 1:
                        a["xma_mixed_banks"] += 1
                pos = max(end, i + 4)
            m.close()
        for k in ("by_codec", "by_kind", "music_channels_by_codec"):
            a[k] = dict(a[k])
        per_archive[name] = a
        print("%s: %d banks, %d streams, %.0f s" % (name, a["banks"], a["streams"], time.time() - t0), flush=True)
    rows = [{"codec": c, "kind": k, "streams": v[0], "bytes": v[1], "seconds": round(v[2], 3)}
            for (_, c, k), v in sorted(totals.items())]
    with open(os.path.join(out_dir, "census.json"), "w") as f:
        json.dump({"archives": per_archive, "totals": rows}, f, indent=1)
    with open(os.path.join(out_dir, "xma_streams.csv"), "w", newline="") as f:
        if xma_rows:
            w = csv.DictWriter(f, fieldnames=list(xma_rows[0]))
            w.writeheader()
            w.writerows(xma_rows)
    print("xma streams: %d" % len(xma_rows))


if __name__ == "__main__":
    main()
