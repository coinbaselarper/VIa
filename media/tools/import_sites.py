"""List call/load sites of selected imports in a PE and disassemble a window around each.

usage: python -I import_sites.py PE DLLSUBSTR... [--before N] [--after N]
Read-only: opens the PE, prints text.
"""
import argparse
import re
import capstone
import pefile


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pe")
    ap.add_argument("dll", nargs="+")
    ap.add_argument("--before", type=lambda s: int(s, 0), default=0x60)
    ap.add_argument("--after", type=lambda s: int(s, 0), default=0x80)
    a = ap.parse_args()
    pe = pefile.PE(a.pe, fast_load=False)
    base = pe.OPTIONAL_HEADER.ImageBase
    iat = {}
    for d in pe.DIRECTORY_ENTRY_IMPORT:
        dn = d.dll.decode().lower()
        if not any(s.lower() in dn for s in a.dll):
            continue
        for imp in d.imports:
            nm = imp.name.decode() if imp.name else f"#{imp.ordinal}"
            iat[imp.address] = f"{dn}!{nm}"
    print("IAT slots:")
    for k, v in sorted(iat.items()):
        print(f"  {k:#x} {v}")
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    pat = re.compile(rb"\xff[\x15\x25]|[\x48\x4c][\x8b][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", re.S)
    for sec in pe.sections:
        if not sec.Characteristics & 0x20000000:
            continue
        data = sec.get_data()
        va0 = base + sec.VirtualAddress
        for m in pat.finditer(data):
            i = m.start()
            n = 2 if data[i] == 0xFF else 3
            disp = int.from_bytes(data[i + n:i + n + 4], "little", signed=True)
            tgt = va0 + i + n + 4 + disp
            if tgt not in iat:
                continue
            site = va0 + i
            print(f"\n=== {iat[tgt]} referenced at {site:#x} (rva {site - base:#x})")
            s = max(0, i - a.before)
            for ins in md.disasm(data[s:i + a.after], va0 + s):
                mark = ">>" if ins.address == site else "  "
                print(f"{mark} {ins.address:#x}: {ins.mnemonic} {ins.op_str}")


main()
