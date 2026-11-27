#!/usr/bin/env python3
"""MSL passes that shrink translated shaders' native code (pipeline memory) and thread-private scratch, each exact by
construction.

  hash_noinline: the bindless lookup (FNV-1a of a T#/S#, then up to 8 table probes) is emitted always_inline at every
  call site (182 copies in a G-buffer pixel shader: 68% of its native code). The probe function becomes noinline; a
  call returns exactly what the inlined copy computes (pure in its arguments and the table, which no draw changes
  while it runs). With hash_memo's wrapper (mspeed rows), only the probe (`<hash>_mspeed`) goes out of line, so a memo
  hit stays inline.

  wired2_passes.py MSL_IN MSL_OUT [pass,...]     (default: hash_noinline)
"""
import re
import sys

ATTR = "static inline __attribute__((always_inline))"
FNV = "2166136261u"


def functions(msl):
    out = {}
    for m in re.finditer(r"^(?:static inline __attribute__\(\(always_inline\)\)\n|(?:static )?__attribute__\(\(noinline\)\)\n|static inline )?"
                         r"([\w:<>, ]+?)\s+(\w+)\(([^;{]*?)\)\s*\n\{", msl, re.M):
        start = m.start()
        depth, i = 0, m.end() - 1
        while i < len(msl):
            c = msl[i]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        out[m.group(2)] = (start, m.end() - 1, i + 1)
    return out


def hash_noinline(msl):
    f = functions(msl)
    hs = [n for n, (s, bs, be) in f.items() if FNV in msl[bs:be] and n != "main0"]
    if not hs:
        return msl, {"hash_noinline": 0, "why": "no lookup function"}
    if len(hs) != 1:
        return msl, {"hash_noinline": 0, "why": "%d lookup functions" % len(hs)}
    h = hs[0]
    s, bs, be = f[h]
    head = msl[s:bs]
    if not head.startswith(ATTR + "\n"):
        if head.startswith(("__attribute__((noinline))", "static __attribute__((noinline))")):
            return msl, {"hash_noinline": 0, "why": "already noinline"}
        if head.startswith("static inline "):
            new = "static __attribute__((noinline))\n" + head[len("static inline "):]
        else:
            return msl, {"hash_noinline": 0, "why": "unexpected declaration"}
    else:
        new = "static __attribute__((noinline))\n" + head[len(ATTR) + 1:]
    calls = len(re.findall(r"(?<![\w.])" + re.escape(h) + r"\(", msl)) - 1
    return msl[:s] + new + msl[bs:], {"hash_noinline": 1, "function": h, "call_sites": calls}


INT_TYPE = re.compile(r"^(?:const\s+)?(?:u?int|bool|u?short|u?char|u?long)[234]?$")
DEVICE_REF = re.compile(r"^(?:const\s+)?device\s+[\w:<>]+\s*&$")


def int_signature(ret, params):
    """True when the helper's return and every parameter are integer scalars/vectors (by value) or device
    references: a call then computes exactly what the inlined copy does (no float contraction can cross it)."""
    if not INT_TYPE.match(ret.strip()):
        return False
    for p in [x.strip() for x in params.split(",") if x.strip()]:
        m = re.match(r"(.*?)\s*\b(\w+)$", p)
        if not m:
            return False
        t = m.group(1).strip()
        if INT_TYPE.match(t) or DEVICE_REF.match(t):
            continue
        return False
    return True


def int_helpers_noinline(msl, min_calls=2, min_chars=300):
    """Every always_inline helper with an integer-only signature (the translator's typed-buffer format decoders,
    register-indexed reads, the bindless lookup) that is called min_calls times or more and is min_chars long goes out
    of line. hash_memo's wrapper (a thread pointer parameter) stays inline."""
    f = functions(msl)
    done, out = [], msl
    for name, (s, bs, be) in sorted(f.items(), key=lambda kv: -kv[1][0]):
        if name == "main0":
            continue
        head = msl[s:bs]
        if not head.startswith(ATTR + "\n"):
            continue
        m = re.match(re.escape(ATTR) + r"\n([\w:<>, ]+?)\s+(\w+)\((.*)\)\s*$", head, re.S)
        if not m or not int_signature(m.group(1), m.group(3)):
            continue
        calls = len(re.findall(r"(?<![\w.])" + re.escape(name) + r"\(", msl)) - 1
        if calls < min_calls or be - bs < min_chars:
            continue
        done.append((name, calls, be - bs))
    for name, calls, size in done:
        s, bs, be = functions(out)[name]
        out = out[:s] + "static __attribute__((noinline))\n" + out[s + len(ATTR) + 1:]
    return out, {"int_helpers_noinline": len(done), "helpers": [d[0] for d in done][:20],
                 "call_sites": sum(d[1] for d in done)}


PASSES = {"hash_noinline": hash_noinline, "int_helpers_noinline": int_helpers_noinline}


def main():
    src, dst = sys.argv[1], sys.argv[2]
    passes = sys.argv[3].split(",") if len(sys.argv) > 3 else ["hash_noinline"]
    msl = open(src).read()
    info = {}
    for p in passes:
        msl, i = PASSES[p](msl)
        info[p] = i
    open(dst, "w").write(msl)
    print(info)


if __name__ == "__main__":
    main()
