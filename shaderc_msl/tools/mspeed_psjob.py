#!/usr/bin/env python3
"""Fragment-shader jobs for mspeed_probe (mode ps): a full-screen triangle whose vertex function writes every user
input of the fragment (per-vertex hash values, interpolated), 32-bit colour targets for every output, the bindings
read from the MSL.

  mspeed_psjob.py FRAGMENT.metal OUT.json INPUTPREFIX|zero REPS DRAWS LIB...
Writes OUT.json and OUT.vs.metal; outputs land in OUT.<lib>.out (targets in order, raw texels).
"""
import json
import re
import sys

KINDS = {"texture2d": "tex2d", "texture1d": "tex1d", "texture3d": "tex3d", "texture2d_array": "tex2darr",
         "texturecube": "texcube", "sampler": "sampler"}
FMT = {"float4": 125, "half4": 125, "uint4": 123, "int4": 124}


def plan(msl):
    args = {}
    for m in re.finditer(r"struct spvDescriptorSetBuffer(\d+)\n\{\n(.*?)\n\};", msl, re.S):
        first = m.group(2).split("\n")[0]
        t = re.search(r"spvDescriptor<(\w+)<", first) or re.search(r"spvDescriptor<(sampler)>", first)
        if t:
            args[m.group(1)] = KINDS.get(t.group(1), "tex2d")
        elif "const device" in first:
            args[m.group(1)] = "table"
    sig = re.search(r"fragment \w+ main0\((.*)\)", msl).group(1)
    push = re.search(r"constant \w+& \w+ \[\[buffer\((\d+)\)\]\]", sig)
    ins = re.findall(r"^\s*(\w+) (\w+) \[\[user\(locn(\d+)\)(?:, (\w+))?\]\];", re.search(r"struct main0_in\n\{\n(.*?)\n\};", msl, re.S).group(1), re.M) \
        if "struct main0_in" in msl else []
    outs = re.findall(r"^\s*(\w+) \w+ \[\[color\((\d+)\)\]\];", re.search(r"struct main0_out\n\{\n(.*?)\n\};", msl, re.S).group(1), re.M)
    return args, int(push.group(1)) if push else 0, ins, outs


def vertex_source(ins):
    lines = ["#include <metal_stdlib>", "using namespace metal;", "struct VO", "{", "    float4 p [[position]];"]
    for t, n, loc, interp in ins:
        lines.append("    %s v%s [[user(locn%s)]];" % (t, loc, loc))
    lines += ["};", "static float h(uint x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return float(x & 0xffffu) / 65535.0; }",
              "vertex VO mspeed_v(uint vid [[vertex_id]])", "{", "    VO o;",
              "    float2 c = float2(vid == 1u ? 3.0 : -1.0, vid == 2u ? 3.0 : -1.0);", "    o.p = float4(c, 0.5, 1.0);"]
    for t, n, loc, interp in ins:
        k = int(loc)
        if t.startswith("float") or t.startswith("half"):
            lines.append("    o.v%d = %s(h(vid * 97u + %du), h(vid * 89u + %du), h(vid * 83u + %du), h(vid * 79u + %du));" % (k, t, k, k + 31, k + 57, k + 101))
        else:
            lines.append("    o.v%d = %s(vid * 7u + %du);" % (k, t, k))
    lines += ["    return o;", "}"]
    return "\n".join(lines) + "\n"


def main():
    frag, out, pre, reps, draws = sys.argv[1:6]
    libs = sys.argv[6:]
    msl = open(frag).read()
    args, push, ins, outs = plan(msl)
    vs = out[:-5] + ".vs.metal"
    open(vs, "w").write(vertex_source(ins))
    j = {"mode": "ps", "libs": libs, "entry": "main0", "push_index": push, "heap_bytes": 67108864, "argbufs": args,
         "textures": 16, "vertex_src": vs, "colors": [FMT.get(t, 125) for t, i in sorted(outs, key=lambda x: int(x[1]))],
         "width": 256, "height": 256, "reps": int(reps), "draws": int(draws), "out_prefix": out[:-5]}
    if pre == "zero":
        j["push"] = [0] * 36
    else:
        j["push"] = json.load(open(pre + ".push.json"))
        j["heap_image"] = pre + ".heap"
        j["hash_words"] = pre + ".hash"
    json.dump(j, open(out, "w"))
    print(json.dumps({k: v for k, v in j.items() if k in ("argbufs", "push_index", "colors")}), len(ins), "inputs")


if __name__ == "__main__":
    main()
