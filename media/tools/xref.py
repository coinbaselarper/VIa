"""Find direct call/jmp rel32 and lea/mov rip-relative references to target VAs in a PE.

usage: python -I xref.py PE VA[=label]... [--ctx N]   (VAs hex). Prints each site with a disassembly window.
Read-only.
"""
import argparse
import re
import capstone
import pefile

ap = argparse.ArgumentParser()
ap.add_argument("pe")
ap.add_argument("targets", nargs="+")
ap.add_argument("--before", type=lambda s: int(s, 0), default=0x40)
ap.add_argument("--after", type=lambda s: int(s, 0), default=0x30)
ap.add_argument("--no-disasm", action="store_true")
a = ap.parse_args()
tg = {}
for t in a.targets:
    va, _, lab = t.partition("=")
    tg[int(va, 16)] = lab or va
pe = pefile.PE(a.pe, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
rel = re.compile(rb"[\xe8\xe9]", re.S)
riprel = re.compile(rb"[\x48\x4c][\x8d\x8b][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", re.S)
for sec in pe.sections:
    if not sec.Characteristics & 0x20000000:
        continue
    data = sec.get_data()
    va0 = base + sec.VirtualAddress
    hits = []
    for m in rel.finditer(data):
        i = m.start()
        tgt = va0 + i + 5 + int.from_bytes(data[i + 1:i + 5], "little", signed=True)
        if tgt in tg:
            hits.append((i, tgt))
    for m in riprel.finditer(data):
        i = m.start()
        tgt = va0 + i + 7 + int.from_bytes(data[i + 3:i + 7], "little", signed=True)
        if tgt in tg:
            hits.append((i, tgt))
    for i, tgt in sorted(hits):
        site = va0 + i
        print(f"=== {tg[tgt]} from {site:#x} (rva {site - base:#x})")
        if a.no_disasm:
            continue
        s = max(0, i - a.before)
        for ins in md.disasm(data[s:i + a.after], va0 + s):
            mark = ">>" if ins.address == site else "  "
            print(f"{mark} {ins.address:#x}: {ins.mnemonic} {ins.op_str}")
