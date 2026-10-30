# Short-lived regions in reserves_<session>.csv (ReserveLog=1): regions of at least --min-kb
# that were released within --max-s seconds, grouped by the first frames in non-system modules,
# for the brief free-space dips the samples show. --placed lists only regions reserved at a
# chosen address (type has 0x40000000).
# Usage: python -I resspikes.py <reserves csv> [--min-kb N] [--max-s S] [--from S] [--frames N] [--placed]
import csv, collections, argparse

SYSTEM = ('ntdll.dll', 'KERNELBASE.dll', 'KERNEL32.DLL', 'kernel32.dll', 'MSIMG32.DLL', 'msimg32.dll')

def signature(frames, n):  # as in resdiff.py
    out = [f for f in frames if not f.startswith(SYSTEM)]
    return ' < '.join(out[:n]) if out else '(system only) ' + ' '.join(frames[:3])

ap = argparse.ArgumentParser()
ap.add_argument('csv')
ap.add_argument('--min-kb', type=int, default=8192)
ap.add_argument('--max-s', type=float, default=10)
ap.add_argument('--from', dest='t0', type=float, default=0)
ap.add_argument('--frames', type=int, default=4)
ap.add_argument('--placed', action='store_true')
a = ap.parse_args()

live = {}
groups = collections.defaultdict(lambda: [0, 0, []])  # count, KB, times
for r in csv.DictReader(open(a.csv)):
    if None in r.values() or not r['frames']:
        continue  # the last row, cut short when the game ended
    t = float(r['t_s'])
    base = int(r['base'], 16)
    if r['op'] == 'R':
        typ = int(r['type'], 16)
        live[base] = (t, int(r['size_kb']), typ, signature(r['frames'].split(), a.frames))
    elif base in live:
        t_r, kb, typ, sig = live.pop(base)
        if t_r < a.t0 or kb < a.min_kb or t - t_r > a.max_s or (a.placed and not typ & 0x40000000):
            continue
        g = groups[sig]
        g[0] += 1; g[1] += kb; g[2].append(t_r)

print('Regions >= %d KB released within %g s (from %g s)' % (a.min_kb, a.max_s, a.t0))
for sig, (n, kb, ts) in sorted(groups.items(), key=lambda kv: -kv[1][1]):
    print('%5d x  %8.1f MB total, %6.1f MB avg  %s' % (n, kb / 1024, kb / 1024 / n, sig))
    print('        at s: %s%s' % (' '.join('%.0f' % t for t in ts[:12]), ' ...' if len(ts) > 12 else ''))
