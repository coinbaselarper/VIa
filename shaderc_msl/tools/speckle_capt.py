#!/usr/bin/env python3
"""T#s of a GTA6RT_METAL_CS_CAPTURE file (CSCAP001): the kernel's user data, and the 8 descriptor words at each byte
OFFSET of the table that user-data register UDREG points at (32-bit pointer, matched against the captured tables),
decoded (address, GFX10 format code, size, dst_sel, type).

  speckle_capt.py CAP UDREG OFFSET...      e.g. speckle_capt.py library_lib_cs_01107_0.cap 5 0x20 0x40
"""
import sys, struct
d = open(sys.argv[1], 'rb').read()
assert d[:8] == b'CSCAP001'
o = 8; n = struct.unpack_from('<I', d, o)[0]; o += 4; label = d[o:o + n].decode(); o += n
gx, gy, gz, ind = struct.unpack_from('<4I', d, o); o += 16
ud = struct.unpack_from('<32I', d, o); o += 128
nr = struct.unpack_from('<I', d, o)[0]; o += 4
regions = []
for i in range(nr):
    va, sz = struct.unpack_from('<QQ', d, o); o += 16
    kind = struct.unpack_from('<I', d, o)[0]; o += 4
    regions.append((va, kind, d[o:o + sz])); o += sz
print(label, 'groups', gx, gy, gz, 'indirect', ind)
print('user data', ' '.join('%08x' % w for w in ud[:16]))
print('regions', [(hex(va), k, len(b)) for va, k, b in regions][:20])
def read(va, nbytes):
    for rva, k, b in regions:
        if rva <= va and va + nbytes <= rva + len(b):
            return b[va - rva: va - rva + nbytes]
    return None
SEL = ['0', '1', '?', '?', 'X', 'Y', 'Z', 'W']
def tsharp(w):
    addr = (w[0] | ((w[1] & 0xff) << 32)) << 8
    fmt = (w[1] >> 20) & 0x1ff
    width = (((w[2] & 0xfff) << 2) | (w[1] >> 30)) + 1
    height = ((w[2] >> 14) & 0x3fff) + 1
    sel = ''.join(SEL[(w[3] >> s) & 7] for s in (0, 3, 6, 9))
    return 'addr 0x%x format %d %dx%d dst_sel %s type %d' % (addr, fmt, width, height, sel, w[3] >> 28)
reg = int(sys.argv[2])
table = ud[reg] | (0 << 32)
for off in sys.argv[3:]:
    off = int(off, 0)
    # the table pointer: s[reg] low, high 0 (s_mov s9, 0) on these kernels; try both the plain value and regions
    b = None
    for rva, k, rb in regions:
        if (rva & 0xffffffff) <= table + off < (rva & 0xffffffff) + len(rb) and (rva >> 32) in (0, rva >> 32):
            b = read((rva & ~0xffffffff) | (table + off), 32)
            if b is not None:
                print('table region va 0x%x kind %d' % (rva, k))
                break
    if b is None:
        print('offset 0x%x: table 0x%x not in the capture' % (off, table)); continue
    w = struct.unpack('<8I', b)
    print('offset 0x%x: %s | %s' % (off, ' '.join('%08x' % x for x in w), tsharp(w)))
