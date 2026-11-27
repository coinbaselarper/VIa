#!/usr/bin/env python3
"""ISA-relevant census of guest DLLs: gs: operands by offset, insertq/extrq, cpuid/xgetbv, vpsrlvq/vpsllvq,
rd/wr[fg]sbase, and the MSVC __chkstk byte signature. Linear sweep over executable sections (1-byte resync on
undecodable bytes, int3 padding skipped), so counts are an upper bound on real code sites.
usage: dll_census.py OUT.json DLL..."""
import sys, json, re, hashlib, collections
import pefile, capstone

WANT = ('insertq', 'extrq', 'cpuid', 'xgetbv', 'vpsrlvq', 'vpsllvq', 'rdgsbase', 'wrgsbase', 'rdfsbase', 'wrfsbase')
CHKSTK = bytes.fromhex('4c8914244c895c24084d33db4c8d5424184c2bd04d0f42d3654c8b1c2510000000')
GS = re.compile(r'gs:\[([^\]]+)\]')


def scan(path):
    pe = pefile.PE(path, fast_load=True)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    res = dict(sha256=hashlib.sha256(open(path, 'rb').read()).hexdigest(), gs=collections.Counter(), mn=collections.Counter(),
               sites=[], chkstk=[])
    for s in pe.sections:
        if not s.Characteristics & 0x20000000:
            continue
        data = s.get_data()[:s.Misc_VirtualSize]
        k = data.find(CHKSTK)
        while k >= 0:
            if k >= 4 and data[k - 4:k] == bytes.fromhex('4883ec10'):
                res['chkstk'].append(s.VirtualAddress + k - 4)
            k = data.find(CHKSTK, k + 1)
        off = 0
        while off < len(data):
            for addr, size, mn, op in md.disasm_lite(data[off:], s.VirtualAddress + off):
                g = GS.search(op)
                if g:
                    res['gs'][g.group(1)] += 1
                if mn in WANT:
                    res['mn'][mn] += 1
                    res['sites'].append(dict(rva=addr, mn=mn, op=op, bytes=data[addr - s.VirtualAddress:addr - s.VirtualAddress + size].hex()))
                elif g:
                    res['sites'].append(dict(rva=addr, mn=mn, op=op, bytes=data[addr - s.VirtualAddress:addr - s.VirtualAddress + size].hex()))
                off = addr - s.VirtualAddress + size
            while off < len(data) and data[off] == 0xCC:
                off += 1
            if off < len(data):
                off += 1
    res['gs'] = dict(res['gs'])
    res['mn'] = dict(res['mn'])
    return res


def main():
    out = {}
    for p in sys.argv[2:]:
        name = p.rsplit('/', 1)[-1]
        r = scan(p)
        out[name] = r
        print('%-32s gs=%s mn=%s chkstk=%s' % (name, r['gs'], r['mn'], [hex(x) for x in r['chkstk']]))
    json.dump(out, open(sys.argv[1], 'w'), indent=1)


if __name__ == '__main__':
    main()
