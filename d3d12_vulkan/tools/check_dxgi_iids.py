"""Check every __CRT_UUID_DECL IID in include/dxgi1_6.h against the GUID bytes in given binaries (e.g. Wine's dxgi.dll).

    python3 -I -B check_dxgi_iids.py <dxgi1_6.h> <binary> [<binary> ...]
"""
import re
import struct
import sys

DECL = re.compile(r"__CRT_UUID_DECL\((\w+),\s*([^)]*)\)")


def main():
    header, binaries = sys.argv[1], sys.argv[2:]
    blobs = [open(b, "rb").read() for b in binaries]
    missing = 0
    for name, parts in DECL.findall(open(header).read()):
        v = [int(x, 16) for x in parts.split(",")]
        guid = struct.pack("<IHH8B", v[0], v[1], v[2], *v[3:])
        hits = [binaries[i].split("/")[-3] + "/" + binaries[i].split("/")[-2] for i, b in enumerate(blobs) if guid in b]
        print(f"{name:26s} {'found in ' + ', '.join(hits) if hits else 'NOT FOUND'}")
        missing += not hits
    print(f"{missing} not found")


if __name__ == "__main__":
    main()
