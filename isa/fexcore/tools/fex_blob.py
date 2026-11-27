# Turns an x86_64 Mach-O object's __text section into a C header: the bytes (pc-relative relocations to symbols in
# the same section resolved) and the offset of every global _g_* symbol. Origin: spike/x6-fexcore/harness/mkblob.py.
import struct
import sys


def main(obj_path, out_path):
    data = bytearray(open(obj_path, "rb").read())
    magic, cputype, _, filetype, ncmds, _, _, _ = struct.unpack_from("<IiiIIIII", data, 0)
    assert magic == 0xFEEDFACF and cputype == 0x01000007 and filetype == 1
    off = 32
    text = None
    symtab = None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, off)
        if cmd == 0x19:
            nsects = struct.unpack_from("<I", data, off + 64)[0]
            s = off + 72
            for i in range(nsects):
                sectname = data[s:s + 16].rstrip(b"\0").decode()
                addr, sz, foff, _, reloff, nreloc = struct.unpack_from("<QQIIII", data, s + 32)
                if sectname == "__text":
                    text = (i + 1, addr, sz, foff, reloff, nreloc)
                s += 80
        elif cmd == 0x2:
            symtab = struct.unpack_from("<IIII", data, off + 8)
        off += size
    assert text and symtab
    sect_index, sect_addr, size, foff, reloff, nreloc = text
    blob = bytearray(data[foff:foff + size])
    symoff, nsyms, stroff, _ = symtab
    syms = []
    for i in range(nsyms):
        strx, ntype, nsect, _, value = struct.unpack_from("<IBBhQ", data, symoff + i * 16)
        end = data.index(b"\0", stroff + strx)
        syms.append((data[stroff + strx:end].decode(), ntype, nsect, value))
    for i in range(nreloc):
        address, info = struct.unpack_from("<iI", data, reloff + i * 8)
        symnum = info & 0xFFFFFF
        pcrel = (info >> 24) & 1
        length = (info >> 25) & 3
        extern = (info >> 27) & 1
        rtype = info >> 28
        if not extern:
            continue
        assert pcrel == 1 and length == 2 and rtype in (1, 2), (address, info)
        name, _, nsect, value = syms[symnum]
        assert nsect == sect_index, name
        addend = struct.unpack_from("<i", blob, address)[0]
        disp = (value - sect_addr + addend) - (address + 4)
        struct.pack_into("<i", blob, address, disp)
    with open(out_path, "w") as f:
        f.write("#pragma once\n#include <stddef.h>\n#include <stdint.h>\n")
        f.write("static const uint8_t kGuestBlob[%d] = {\n" % len(blob))
        for i in range(0, len(blob), 16):
            f.write("  " + ", ".join("0x%02x" % b for b in blob[i:i + 16]) + ",\n")
        f.write("};\n")
        for name, ntype, nsect, value in sorted(syms, key=lambda s: s[3]):
            if nsect == sect_index and (ntype & 1) and name.startswith("_g_"):
                f.write("static const size_t kOff%s = 0x%x;\n" % (name[2:], value - sect_addr))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
