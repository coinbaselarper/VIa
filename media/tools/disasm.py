"""Disassemble a VA range of a PE: python -I disasm.py PE START END (hex VAs). Read-only."""
import sys
import capstone
import pefile

pe = pefile.PE(sys.argv[1], fast_load=False)
base = pe.OPTIONAL_HEADER.ImageBase
start, end = int(sys.argv[2], 16), int(sys.argv[3], 16)
names = {}
for d in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
    for imp in d.imports:
        names[imp.address] = d.dll.decode() + "!" + (imp.name.decode() if imp.name else f"#{imp.ordinal}")
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.skipdata = True
data = pe.get_memory_mapped_image()[start - base:end - base]
for ins in md.disasm(data, start):
    note = ""
    if "rip +" in ins.op_str or "rip -" in ins.op_str:
        try:
            off = ins.op_str.split("rip ")[1].split("]")[0].replace(" ", "")
            tgt = ins.address + ins.size + int(off, 16)
            note = f"   ; {tgt:#x}" + (f" {names[tgt]}" if tgt in names else "")
        except Exception:
            pass
    print(f"{ins.address:#x}: {ins.mnemonic} {ins.op_str}{note}")
