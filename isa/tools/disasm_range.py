#!/usr/bin/env python3
"""disasm_range.py EXE RVA_START RVA_END : linear disassembly of an RVA range of .text (capstone)."""
import sys, pefile, capstone
pe = pefile.PE(sys.argv[1], fast_load=True)
t = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text')
a, b = int(sys.argv[2], 16), int(sys.argv[3], 16)
code = t.get_data()[a - t.VirtualAddress:b - t.VirtualAddress]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for i in md.disasm(code, a):
    print('%8x  %-22s %s %s' % (i.address, i.bytes.hex(), i.mnemonic, i.op_str))
