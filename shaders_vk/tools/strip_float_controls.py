#!/usr/bin/env python3
"""Turn modules the unmodified translator wrote into what --apple-float-controls writes (port/shaders_vk).

--apple-float-controls changes only the preamble: no OpCapability DenormPreserve, no OpExecutionMode DenormPreserve 16
and no OpExecutionMode SignedZeroInfNanPreserve 64 (that mode's capability, SignedZeroInfNanPreserve, stays for the
32-bit one). Neither instruction names an id, so removing them leaves every id and the header unchanged; the result is
byte-identical to a re-translation, which --verify checks against a re-translated folder.

  strip_float_controls.py --src OLD --out NEW            every .spv and index.json of OLD into NEW
  strip_float_controls.py --src OLD --verify RETRANSLATED   compare strip(OLD) with RETRANSLATED, module by module
"""
import argparse
import json
import os
import struct
import sys

OP_CAPABILITY, OP_EXECUTION_MODE, OP_FUNCTION = 17, 16, 54
CAP_DENORM_PRESERVE = 4464
MODE_DENORM_PRESERVE, MODE_SIGNED_ZERO_INF_NAN = 4459, 4461


def strip(data):
    words = struct.unpack(f"<{len(data) // 4}I", data)
    if len(words) < 5 or words[0] != 0x07230203:
        raise ValueError("not SPIR-V")
    out = list(words[:5])
    i = 5
    while i < len(words):
        n, op = words[i] >> 16, words[i] & 0xFFFF
        if not n or i + n > len(words):
            raise ValueError("bad instruction")
        ins = words[i:i + n]
        drop = (op == OP_CAPABILITY and n == 2 and ins[1] == CAP_DENORM_PRESERVE) or \
               (op == OP_EXECUTION_MODE and n == 4 and
                ((ins[2] == MODE_DENORM_PRESERVE and ins[3] == 16) or (ins[2] == MODE_SIGNED_ZERO_INF_NAN and ins[3] == 64)))
        if op == OP_FUNCTION:
            out.extend(words[i:])
            break
        if not drop:
            out.extend(ins)
        i += n
    return struct.pack(f"<{len(out)}I", *out)


def strip_meta(meta):
    m = dict(meta)
    m["capabilities"] = [c for c in m.get("capabilities", []) if c != "DenormPreserve"]
    if "float_controls" in m:
        m["float_controls"] = [f for f in m["float_controls"]
                               if f not in ("shaderDenormPreserveFloat16", "shaderSignedZeroInfNanPreserveFloat64")]
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--out")
    ap.add_argument("--verify")
    ap.add_argument("--skip-stage", action="append", default=[], help="with --verify: stages left out (vs)")
    a = ap.parse_args()
    with open(os.path.join(a.src, "index.json")) as f:
        index = json.load(f)
    if a.verify:
        with open(os.path.join(a.verify, "index.json")) as f:
            other = json.load(f)
        same = differ = missing = meta_differ = 0
        examples = []
        for name, meta in index.items():
            if meta.get("stage") in a.skip_stage:
                continue
            p = os.path.join(a.verify, name + ".spv")
            if name not in other or not os.path.exists(p):
                missing += 1
                continue
            with open(os.path.join(a.src, name + ".spv"), "rb") as f:
                mine = strip(f.read())
            with open(p, "rb") as f:
                theirs = f.read()
            if mine == theirs:
                same += 1
            else:
                differ += 1
                if len(examples) < 5:
                    examples.append(name)
            if strip_meta(meta) != other[name]:
                meta_differ += 1
        print(f"verify: {same} byte-identical, {differ} differ, {missing} not in the re-translation; "
              f"index entries differing: {meta_differ}; first differing: {examples}")
        return 1 if differ else 0
    os.makedirs(a.out, exist_ok=True)
    out_index = {}
    for name, meta in index.items():
        with open(os.path.join(a.src, name + ".spv"), "rb") as f:
            data = strip(f.read())
        with open(os.path.join(a.out, name + ".spv"), "wb") as f:
            f.write(data)
        out_index[name] = strip_meta(meta)
    with open(os.path.join(a.out, "index.json"), "w") as f:
        json.dump(out_index, f, sort_keys=True)
    print(f"stripped {len(out_index)} modules into {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
