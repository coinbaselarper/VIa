import sys

# usage: frame_log_summary.py FRAMES.tsv [--window T0 T1] [--long N]
path = sys.argv[1]
args = sys.argv[2:]
window = None
long_n = 12
if '--window' in args:
    i = args.index('--window'); window = (float(args[i + 1]), float(args[i + 2]))
if '--long' in args:
    long_n = int(args[args.index('--long') + 1])
cols = None
rows = []
for line in open(path):
    if line.startswith('#'):
        cols = line[1:].split()
        continue
    v = line.split()
    if len(v) != len(cols):
        continue
    rows.append({c: float(x) for c, x in zip(cols, v)})
if not rows:
    print('no rows'); sys.exit()
movie = [r for r in rows if r['movie_new'] > 0]
if window is None:
    if not movie:
        print('no movie frames'); sys.exit()
    window = (movie[0]['t_s'], movie[-1]['t_s'])
sel = [r for r in rows if window[0] < r['t_s'] <= window[1]]
dts = [r['dt_ms'] for r in sel]
def hist(vals, edges):
    out = []
    for a, b in zip(edges, edges[1:]):
        out.append('%g-%g:%d' % (a, b, sum(1 for v in vals if a <= v < b)))
    out.append('>=%g:%d' % (edges[-1], sum(1 for v in vals if v >= edges[-1])))
    return ' '.join(out)
edges = [0, 8.4, 16.7, 25, 33.4, 41.7, 50, 66.7, 100]
span = window[1] - window[0]
print('window %.2f-%.2f s (%.1f s): %d presents, %.1f/s' % (window[0], window[1], span, len(sel), len(sel) / span if span else 0))
if dts:
    s = sorted(dts)
    print('present interval ms: mean %.1f median %.1f p90 %.1f p99 %.1f max %.1f; >33.4: %d, >50: %d' % (
        sum(s) / len(s), s[len(s) // 2], s[int(len(s) * .9)], s[min(len(s) - 1, int(len(s) * .99))], s[-1],
        sum(1 for v in s if v > 33.4), sum(1 for v in s if v > 50)))
    print('  histogram', hist(dts, edges))
mv = [r for r in sel if r['movie_new'] > 0]
if len(mv) > 1:
    mi = [(b['t_s'] - a['t_s']) * 1e3 for a, b in zip(mv, mv[1:])]
    s = sorted(mi)
    total_new = sum(r['movie_new'] for r in sel)
    print('movie frames shown: %d presents carried a new frame (%d set switches); interval ms mean %.1f median %.1f p90 %.1f max %.1f; >34.5: %d, >50: %d' % (
        len(mv), total_new, sum(s) / len(s), s[len(s) // 2], s[int(len(s) * .9)], s[-1],
        sum(1 for v in s if v > 34.5), sum(1 for v in s if v > 50)))
    print('  histogram', hist(mi, edges))
    print('  presents per movie frame: ', ' '.join('%d:%d' % (k, v) for k, v in sorted(
        {k: sum(1 for x in [sum(1 for r in sel if a['t_s'] < r['t_s'] <= b['t_s']) for a, b in zip(mv, mv[1:])] if x == k) for k in range(1, 8)}.items()) if v))
keys = ['subs', 'wait', 'heaps', 'shadows', 'geometry', 'textures', 'replay', 'present', 'fence', 'acquire', 'pipe_wait_ms', 'build_ms', 'tokens', 'token_wait_ms']
if sel:
    print('mean per frame: ' + ' '.join('%s %.2f' % (k, sum(r[k] for r in sel) / len(sel)) for k in keys if k in sel[0]))
    print('longest frames:')
    for r in sorted(sel, key=lambda r: -r['dt_ms'])[:long_n]:
        print('  t %.3f dt %.1f | ' % (r['t_s'], r['dt_ms']) + ' '.join('%s %.1f' % (k, r[k]) for k in keys if k in r and r[k]) + ' movie_new %d' % r['movie_new'])
