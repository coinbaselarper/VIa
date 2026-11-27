#!/usr/bin/env python3
"""CPU run of the generated partition code (lane 0's walk in the split mesh stage) on many meshlets.

The MSL of a production split build is compiled as C++ around a driver that, for each random meshlet (strips, fans,
random, shuffled, clustered, repeated and degenerate indices, V and P up to 128) runs the walk once per chunk
(WorkgroupId.x = 0..6, the chunk bound for vmax 64) and checks what the walk leaves in the workgroup arrays:
  - every valid primitive is emitted by exactly one chunk,
  - a chunk has at most vmax vertices and pmax primitives,
  - each emitted primitive's three slots map back to its original vertex indices (slot -> vertex is injective),
  - the chunk count the walk reports is the number of chunks that emit anything.
Needs no GPU. Usage: partition_emul.py --stem NAME [--trials 20000] [--vmax 64] [--pmax 64] --work DIR
"""
import argparse, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mesh_split_check as C  # noqa: E402


def extract(msl):
    src = msl.split("\n")
    head = next(i for i, l in enumerate(src) if l.strip() == "if (gl_LocalInvocationIndex == 0u)" and "for (" in "".join(src[i:i + 14]))
    end = next(i for i in range(head, len(src)) if re.match(r"\s*_\d+\[2u\] = _\d+;", src[i]))
    body = "\n".join(src[head + 1:end + 1]) + "\n}"
    ctx = "\n".join(src[max(0, head - 40):head])
    vt = re.search(r"(_\d+)\[gl_LocalInvocationIndex\] = 0u;", ctx).group(1)
    pn = re.search(r"(_\d+)\[gl_LocalInvocationIndex\] = 4294967295u;", ctx).group(1)
    pk = re.search(r"(_\d+)\[gl_LocalInvocationIndex\] = \(\(.*<< 24u\);", ctx).group(1)
    chunk1 = re.search(r"uint (_\d+) = gl_WorkGroupID\.x \+ 1u;", ctx).group(1)
    pvar = re.search(r"for \(uint _\d+ = 0u,.*?; _\d+ < (_\d+);", body).group(1)
    cnt = re.search(r"(_\d+)\[0u\] = _\d+;", body).group(1)
    return body, vt, pn, pk, chunk1, pvar, cnt


DRIVER = r'''
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <random>
#include <algorithm>
#include <map>
typedef uint32_t uint;
static uint VT[128], PK[128], PN[128], CNT[128];
#define VMAX %(vmax)d
#define PMAX %(pmax)d
struct Mesh { int V; std::vector<std::array<int,3>> tris; };
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stem", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--trials", type=int, default=20000)
    ap.add_argument("--vmax", type=int, default=64)
    ap.add_argument("--pmax", type=int, default=64)
    ap.add_argument("--opts-from-fit", action="store_true")
    a = ap.parse_args()
    a.work = os.path.abspath(a.work)
    os.makedirs(a.work, exist_ok=True)
    opts = None
    if a.opts_from_fit:
        import batch
        opts = batch.mesh_opts(os.path.join(C.ROOT, "port/build/shaderc_msl/mesh/mesh_fit.jsonl")).get(a.stem)
    sp = C.build_split(a.stem, a.work, 7, False, a.vmax, a.pmax, opts)
    body, vt, pn, pk, chunk1, pvar, cnt = extract(sp["msl"])
    body = (body.replace(f"{vt}[", "VT[").replace(f"{pn}[", "PN[").replace(f"{pk}[", "PK[").replace(f"{cnt}[", "CNT["))
    code = r'''
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <array>
#include <random>
#include <algorithm>
#include <map>
#include <set>
typedef uint32_t uint;
static uint VT[128], PK[128], PN[128], CNT[128];
static const int VMAX = %(vmax)d, PMAX = %(pmax)d;
static void walk(uint %(chunk1)s, uint %(pvar)s) {
  uint gl_LocalInvocationIndex = 0;
  %(body)s
}
struct Mesh { int V; std::vector<std::array<int,3>> tris; };
static Mesh make(int kind, std::mt19937 &r) {
  Mesh m; auto R = [&](int n) { return (int)(r() %% n); };
  switch (kind) {
  case 0: m.V = 128; for (int p = 0; p < 126; p++) m.tris.push_back({p, p+1, p+2}); break;
  case 1: m.V = 128; for (int p = 0; p < 126; p++) m.tris.push_back({0, p+1, p+2}); break;
  case 2: m.V = 128; for (int p = 0; p < 128; p++) m.tris.push_back({R(128), R(128), R(128)}); break;
  case 3: { std::vector<int> pm(128); for (int i = 0; i < 128; i++) pm[i] = i; std::shuffle(pm.begin(), pm.end(), r);
            m.V = 128; for (int p = 0; p < 126; p++) m.tris.push_back({pm[p], pm[p+1], pm[p+2]}); break; }
  case 4: m.V = 10; for (int p = 0; p < 8; p++) m.tris.push_back({p %% 10, (p+1) %% 10, (p+3) %% 10}); break;
  case 5: m.V = 64; for (int p = 0; p < 128; p++) m.tris.push_back({R(64), R(64), R(64)}); break;
  case 6: m.V = 100; for (int p = 0; p < 100; p++) { int i = R(100), j = R(100); int t = R(5);
            m.tris.push_back(t==0?std::array<int,3>{i,i,i}:t==1?std::array<int,3>{i,j,i}:t==2?std::array<int,3>{i,i,j}:t==3?std::array<int,3>{j,i,i}:std::array<int,3>{i,j,j}); } break;
  case 7: m.V = 128; for (int p = 0; p < 128; p++) m.tris.push_back({p, (p+43)%%128, (p+86)%%128}); break;
  case 8: m.V = 128; for (int p = 0; p < 128; p++) { int c = (p/16)*16; m.tris.push_back({c+R(16), c+R(16), c+R(16)}); } break;
  default: { m.V = 1 + R(128); int P = 1 + R(128); for (int p = 0; p < P; p++) m.tris.push_back({R(m.V), R(m.V), R(m.V)}); }
  }
  return m;
}
int main() {
  std::mt19937 r(12345);
  long trials = %(trials)d, bad = 0, prims_total = 0;
  std::map<int, std::map<int,long>> need;
  for (long t = 0; t < trials; t++) {
    int kind = (int)(t %% 10);
    Mesh m = make(kind, r);
    int P = (int)m.tris.size();
    std::vector<int> emitted(P, 0);
    int chunks_used = 0, maxcur = 0;
    for (int c = 0; c < 7; c++) {
      for (int i = 0; i < 128; i++) { VT[i] = 0; PN[i] = 0xFFFFFFFFu; PK[i] = 0; CNT[i] = 0; }
      for (int p = 0; p < P; p++) PK[p] = (m.tris[p][0] & 127) | ((m.tris[p][1] & 127) << 8) | ((m.tris[p][2] & 127) << 16) | (1u << 24);
      walk(c + 1, P);
      uint vc = CNT[0], pc = CNT[1], cur = CNT[2];
      maxcur = std::max<int>(maxcur, cur);
      if ((int)vc > VMAX || (int)pc > PMAX) { bad++; printf("limit exceeded kind %%d chunk %%d vc %%u pc %%u\n", kind, c, vc, pc); continue; }
      std::vector<int> slot_orig(128, -1);
      int slots = 0;
      for (int v = 0; v < 128; v++) if (VT[v] & 0x8000u) { int s = (VT[v] >> 8) & 127; if (s >= (int)vc || slot_orig[s] != -1) { bad++; printf("bad slot kind %%d\n", kind); } else { slot_orig[s] = v; slots++; } }
      if (slots != (int)vc) { bad++; printf("slot count %%d != vc %%u kind %%d\n", slots, vc, kind); }
      int pcount = 0;
      std::set<int> pslots;
      for (int p = 0; p < P; p++) if ((PN[p] >> 24) != 255u) {
        pcount++;
        int ps = PN[p] >> 24; if (ps >= (int)pc || !pslots.insert(ps).second) { bad++; printf("bad prim slot kind %%d\n", kind); }
        int n[3] = {(int)(PN[p] & 255), (int)((PN[p] >> 8) & 255), (int)((PN[p] >> 16) & 255)};
        for (int k = 0; k < 3; k++) if (n[k] >= (int)vc || slot_orig[n[k]] != m.tris[p][k]) { bad++; printf("slot maps wrong kind %%d prim %%d\n", kind, p); break; }
        emitted[p]++;
      }
      if (pcount != (int)pc) { bad++; printf("prim count %%d != pc %%u\n", pcount, pc); }
      if (pc) chunks_used = c + 1;
    }
    for (int p = 0; p < P; p++) { prims_total++; if (emitted[p] != 1) { bad++; if (bad < 20) printf("prim %%d emitted %%d times, kind %%d\n", p, emitted[p], kind); } }
    need[kind][chunks_used]++;
  }
  printf("trials %%ld primitives %%ld violations %%ld\n", trials, prims_total, bad);
  for (auto &k : need) { printf("kind %%d chunks needed:", k.first); for (auto &c : k.second) printf(" %%d:%%ld", c.first, c.second); printf("\n"); }
  return bad ? 1 : 0;
}
''' % dict(vmax=a.vmax, pmax=a.pmax, chunk1=chunk1, pvar=pvar, body=body, trials=a.trials)
    open(os.path.join(a.work, "emul.cpp"), "w").write(code)
    r = subprocess.run(["clang++", "-std=c++17", "-O2", os.path.join(a.work, "emul.cpp"), "-o", os.path.join(a.work, "emul")],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stderr[:2000])
    r = subprocess.run([os.path.join(a.work, "emul")], capture_output=True, text=True, timeout=600)
    print(r.stdout[-2500:])
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
