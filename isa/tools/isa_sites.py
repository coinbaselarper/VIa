#!/usr/bin/env python3
"""Census of the instruction sites isa/ must handle in the exe: insertq/extrq (SSE4a), cpuid, xgetbv,
vpsrlvq/vpsllvq (Rosetta count bug), and the __chkstk gs:[0x10] read. Linear sweep over .pdata ranges and
the gaps between them, like spike/s0.2-avx/census.py.
usage: isa_sites.py EXE OUT.json"""
import sys, struct, json, multiprocessing as mp, hashlib
import pefile, capstone

WANT = ('insertq', 'extrq', 'cpuid', 'xgetbv', 'vpsrlvq', 'vpsllvq')


def load(path):
    pe = pefile.PE(path, fast_load=True)
    text = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text')
    pdata = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.pdata')
    tdata = text.get_data()[:text.Misc_VirtualSize]
    pd = pdata.get_data()[:pdata.Misc_VirtualSize]
    ents = sorted({struct.unpack_from('<III', pd, i) for i in range(0, len(pd) - 11, 12)} - {(0, 0, 0)})
    return pe, text.VirtualAddress, text.PointerToRawData, tdata, ents


def init(tbase, tdata):
    global TBASE, TDATA
    TBASE, TDATA = tbase, tdata


def work(ranges):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    out = []
    for fid, b, e, kind in ranges:
        code = TDATA[b - TBASE:e - TBASE]
        off = 0
        while off < len(code):
            for addr, size, mn, op in md.disasm_lite(code[off:], b + off):
                if mn in WANT or (mn == 'mov' and 'gs:[0x10]' in op):
                    out.append(dict(rva=addr, size=size, bytes=code[addr - b:addr - b + size].hex(), mn=mn, op=op,
                                    kind=kind, range=[b, e]))
                off = addr - b + size
            if off < len(code):
                while off < len(code) and code[off] == 0xCC:
                    off += 1
                else:
                    off += 1 if off < len(code) and code[off] != 0xCC else 0
    return out


def main():
    exe, outp = sys.argv[1], sys.argv[2]
    pe, tbase, traw, tdata, ents = load(exe)
    tend = tbase + len(tdata)
    ranges, cur = [], tbase
    for i, (b, e, u) in enumerate(ents):
        if not (tbase <= b < tend):
            continue
        if b > cur:
            ranges.append((-1, cur, b, 'gap'))
        if e > cur:
            ranges.append((i, max(b, cur), e, 'pdata'))
        cur = max(cur, e)
    if cur < tend:
        ranges.append((-1, cur, tend, 'gap'))
    n = 32
    chunks = [ranges[i::n] for i in range(n)]
    sites = []
    with mp.Pool(4, initializer=init, initargs=(tbase, tdata)) as pool:
        for r in pool.imap_unordered(work, chunks):
            sites += r
    sites.sort(key=lambda s: s['rva'])
    sha = hashlib.sha256(open(exe, 'rb').read()).hexdigest()
    by = {}
    for s in sites:
        by.setdefault(s['mn'], 0)
        by[s['mn']] += 1
    json.dump(dict(exe_sha256=sha, image_base=pe.OPTIONAL_HEADER.ImageBase, text_rva=tbase, text_raw=traw,
                   counts=by, sites=sites), open(outp, 'w'), indent=1)
    print(json.dumps(by))


if __name__ == '__main__':
    main()
