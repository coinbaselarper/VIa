#!/usr/bin/env python3
"""Where f16 denormals can reach a translated module (port/shaders_vk/STATUS.md): per stage, how many modules declare
Float16 or DenormPreserve, and how many contain each instruction that converts between f16 bits and f32
(GLSL.std.450 UnpackHalf2x16 / PackHalf2x16, OpQuantizeToF16, OpFConvert).

  f16_census.py SPIRV_DIR [SPIRV_DIR/part ...]
"""
import json
import os
import struct
import sys
from collections import Counter

OP_CAPABILITY, OP_EXT_INST_IMPORT, OP_EXECUTION_MODE, OP_EXT_INST, OP_FCONVERT, OP_QUANTIZE = 17, 11, 16, 12, 115, 116
CAP_FLOAT16, CAP_DENORM_PRESERVE = 9, 4464
GLSL_PACK_HALF, GLSL_UNPACK_HALF = 58, 62


def scan(path):
    data = open(path, "rb").read()
    words = struct.unpack(f"<{len(data) // 4}I", data)
    found = set()
    glsl = None
    i = 5
    while i < len(words):
        n, op = words[i] >> 16, words[i] & 0xFFFF
        if not n:
            break
        if op == OP_CAPABILITY and words[i + 1] == CAP_FLOAT16:
            found.add("Float16")
        elif op == OP_CAPABILITY and words[i + 1] == CAP_DENORM_PRESERVE:
            found.add("DenormPreserve")
        elif op == OP_EXT_INST_IMPORT:
            name = data[(i + 2) * 4:(i + n) * 4].split(b"\0")[0]
            if name == b"GLSL.std.450":
                glsl = words[i + 1]
        elif op == OP_EXT_INST and words[i + 3] == glsl:
            if words[i + 4] == GLSL_UNPACK_HALF:
                found.add("UnpackHalf2x16")
            elif words[i + 4] == GLSL_PACK_HALF:
                found.add("PackHalf2x16")
        elif op == OP_QUANTIZE:
            found.add("OpQuantizeToF16")
        elif op == OP_FCONVERT:
            found.add("OpFConvert")
        i += n
    return found


def main():
    totals, hits = Counter(), Counter()
    for folder in sys.argv[1:]:
        index = json.load(open(os.path.join(folder, "index.json")))
        part = os.path.basename(folder.rstrip("/"))
        for name, meta in index.items():
            stage = meta.get("stage", "?") if part in ("spirv", "") else f"{part}:{meta.get('stage', '?')}"
            totals[stage] += 1
            for what in scan(os.path.join(folder, name + ".spv")):
                hits[(stage, what)] += 1
    for stage in sorted(totals):
        row = ", ".join(f"{w} {hits[(stage, w)]}" for w in ("Float16", "DenormPreserve", "UnpackHalf2x16",
                                                             "PackHalf2x16", "OpQuantizeToF16", "OpFConvert"))
        print(f"{stage}: {totals[stage]} modules; {row}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
