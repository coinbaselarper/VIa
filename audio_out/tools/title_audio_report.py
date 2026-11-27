"""Movie audio vs video from a title_run.sh capture: python3 -I title_audio_report.py LOG WAV [FROM_S TO_S]

Reads the tap WAV (32-bit float) and the run log ([audio_out] tap frame 0 host time, timestamped [trace] Bink calls,
[rt] audio / [audio_out] stats). Prints per-second BinkNextFrame count, RMS, share of exact-zero frames, cumulative
movie audio frames played vs video frames decoded, and the delay from each decode to the next audio burst."""
import bisect
import math
import re
import struct
import sys


def main():
    log, wav = sys.argv[1], sys.argv[2]
    lines = open(log, errors="replace").read().splitlines()
    t0 = None
    for ln in lines:
        m = re.search(r"tap frame 0 mixed at host (\d+) ns", ln)
        if m:
            t0 = int(m.group(1))
            break
    if t0 is None:
        sys.exit("no tap start line in the log")
    events = []
    for ln in lines:
        m = re.match(r"\[trace\] #\d+ @(\d+) t[0-9a-f]+ bink2scarlett\.dll!(\w+)\(", ln)
        if m:
            events.append(((int(m.group(1)) - t0) / 1e9, m.group(2)))
    nf = sorted(t for t, n in events if n == "BinkNextFrame")
    d = open(wav, "rb").read()
    ch = struct.unpack_from("<H", d, 22)[0]
    rate = struct.unpack_from("<I", d, 24)[0]
    n = (len(d) - 44) // (4 * ch)
    s = struct.unpack_from("<%df" % (n * ch), d, 44)
    a = float(sys.argv[3]) if len(sys.argv) > 3 else max(0.0, (nf[0] if nf else 0) - 3)
    b = float(sys.argv[4]) if len(sys.argv) > 4 else min(n / rate, (nf[-1] if nf else n / rate) + 3)
    print("wav %d ch %d Hz %.3f s; BinkNextFrame %d calls, %.3f..%.3f s (%.2f/s)" % (
        ch, rate, n / rate, len(nf), nf[0] if nf else -1, nf[-1] if nf else -1,
        (len(nf) - 1) / (nf[-1] - nf[0]) if len(nf) > 1 else 0))
    for t, name in events:
        if name in ("BinkRegisterFrameBuffers", "BinkSetSoundSystem") and a - 30 <= t <= b:
            print("  %8.3f s %s" % (t, name))
    print(" second  NextFrame  rms_dBFS  zero%  cum_audio_s  cum_video_s")
    cum = 0
    for sec in range(int(a), int(math.ceil(b))):
        e = z = 0
        lo, hi = sec * rate, min(n, (sec + 1) * rate)
        for i in range(lo, hi):
            x = s[i * ch:i * ch + ch]
            e += sum(v * v for v in x)
            if all(v == 0 for v in x):
                z += 1
            else:
                cum += 1
        rms = math.sqrt(e / max(1, (hi - lo) * ch))
        k = sum(1 for t in nf if sec <= t < sec + 1)
        kc = sum(1 for t in nf if t < sec + 1)
        print("%7d %10d %9.1f %6.1f %12.2f %12.2f" % (sec, k, 20 * math.log10(rms) if rms > 0 else -999,
                                                       100 * z / max(1, hi - lo), cum / rate, kc / 30))
    starts, prev_zero, zl, runs, cur, ln_ = [], True, 0, [], None, 0
    for i in range(int(a * rate), min(n, int(b * rate))):
        zero = all(v == 0 for v in s[i * ch:i * ch + ch])
        if not zero and prev_zero and zl >= 48:
            starts.append(i / rate)
        zl = zl + 1 if zero else 0
        prev_zero = zero
        if zero == cur:
            ln_ += 1
        else:
            if cur is not None:
                runs.append((cur, ln_))
            cur, ln_ = zero, 1
    runs.append((cur, ln_))
    delays = sorted((t - nf[bisect.bisect_right(nf, t) - 1]) * 1000 for t in starts if bisect.bisect_right(nf, t) > 0)
    nz = sorted(l for z, l in runs if not z)
    zz = sorted(l for z, l in runs if z)
    if delays:
        q = lambda v, p: v[min(len(v) - 1, int(p * len(v)))]
        print("audio bursts after a gap: %d; delay from the last BinkNextFrame ms: p10 %.1f median %.1f p90 %.1f max %.1f" % (
            len(delays), q(delays, .1), q(delays, .5), q(delays, .9), delays[-1]))
    if nz:
        print("non-zero runs: %d, median %d frames; zero runs: %d, median %d frames, %d of at least 0.9 s" % (
            len(nz), nz[len(nz) // 2], len(zz), zz[len(zz) // 2] if zz else 0, sum(1 for x in zz if x >= 0.9 * rate)))
    tails = [ln for ln in lines if "[audio_out] periodic" in ln][-3:]
    for ln in tails:
        print(ln[:400])


if __name__ == "__main__":
    main()
