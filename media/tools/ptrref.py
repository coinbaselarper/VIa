"""Find 8-byte absolute pointers to target VAs in a PE's non-code sections: python -I ptrref.py PE VA... Read-only."""
import sys
import pefile

pe = pefile.PE(sys.argv[1], fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
for t in sys.argv[2:]:
    va = int(t, 16)
    needle = va.to_bytes(8, "little")
    for sec in pe.sections:
        data = sec.get_data()
        i = data.find(needle)
        while i >= 0:
            print(f"{va:#x} at {base + sec.VirtualAddress + i:#x} in {sec.Name.rstrip(b'\0').decode()}")
            i = data.find(needle, i + 1)
