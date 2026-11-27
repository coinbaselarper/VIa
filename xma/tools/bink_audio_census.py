"""Count Bink 2 streams and their audio tracks (codec flags, rates) in raw archives. Read-only.

    python3 -I bink_audio_census.py ARCHIVE...

Header layout as in leonida/docs/movies.md (0x2C fixed bytes, audio descriptors after, one extra word before them for
revisions i/j/k/n). Prints one JSON object.
"""
import json
import mmap
import os
import re
import struct
import sys
from collections import Counter

MAGIC = re.compile(rb"KB2[a-z]")


def parse(m, off, size):
    if off + 0x40 > size:
        return None
    stored, frames, largest, _f2, w, h, fn, fd, _extra, audio = struct.unpack_from("<10I", m, off + 4)
    if not (0 < frames < 10_000_000 and 0 < w <= 16384 and 0 < h <= 16384 and fn and fd and audio <= 8):
        return None
    if off + stored + 8 > size or stored < 0x2C:
        return None
    for base in (off + 0x2C, off + 0x30):
        if base + audio * 12 > size:
            continue
        descs = [struct.unpack_from("<HH", m, base + audio * 4 + 4 * i) for i in range(audio)]
        if all(r in (8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000) for r, _ in descs):
            return stored + 8, frames, fn / fd, descs
    return (stored + 8, frames, fn / fd, None) if audio == 0 else None


def main():
    out = {}
    for path in sys.argv[1:]:
        size = os.path.getsize(path)
        streams = with_audio = tracks = 0
        flags, rates, codecs = Counter(), Counter(), Counter()
        seconds = 0.0
        with open(path, "rb") as f:
            m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            pos = 0
            while True:
                hit = MAGIC.search(m, pos)
                if not hit:
                    break
                p = parse(m, hit.start(), size)
                if p is None:
                    pos = hit.start() + 4
                    continue
                length, frames, fps, descs = p
                streams += 1
                if descs:
                    with_audio += 1
                    tracks += len(descs)
                    seconds += frames / fps * len(descs)
                    for r, fl in descs:
                        flags["0x%04x" % fl] += 1
                        rates[r] += 1
                        codecs["bink_audio_dct" if fl & 0x1000 else "bink_audio_rdft"] += 1
                pos = hit.start() + length
            m.close()
        out[os.path.basename(path)] = {"streams": streams, "with_audio": with_audio, "tracks": tracks,
                                       "track_seconds": round(seconds, 1), "flags": flags, "rates": rates,
                                       "codecs": codecs}
    print(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
