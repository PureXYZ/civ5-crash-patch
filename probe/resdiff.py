# Summarises reserves_<session>.csv (ReserveLog=1): address space that was reserved
# in a time window and is still held at the end of the log (or at --at), grouped
# by the first frames in non-system modules.
# Usage: python -I resdiff.py <reserves csv> [--from S] [--to S] [--at S] [--frames N]
import csv, sys, collections, argparse

SYSTEM = ('ntdll.dll', 'KERNELBASE.dll', 'KERNEL32.DLL', 'kernel32.dll', 'MSIMG32.DLL', 'msimg32.dll')

def signature(frames, n):
    out = [f for f in frames if not f.startswith(SYSTEM)]
    return ' < '.join(out[:n]) if out else '(system only) ' + ' '.join(frames[:3])

ap = argparse.ArgumentParser()
ap.add_argument('csv')
ap.add_argument('--from', dest='t0', type=float, default=0)
ap.add_argument('--to', dest='t1', type=float, default=1e18)
ap.add_argument('--at', type=float, default=1e18, help='count releases up to this time')
ap.add_argument('--frames', type=int, default=3)
a = ap.parse_args()

live = {}
made = collections.Counter(); freed = collections.Counter()
KIND = {0x1000: 'commit', 0x2000: 'reserve', 0x3000: 'res+com'}  # MEM_COMMIT / MEM_RESERVE in the type

for r in csv.DictReader(open(a.csv)):
    if None in r.values() or not r['frames']:
        continue  # the last row, cut short when the game ended
    t = float(r['t_s'])
    if t > a.at: break
    base = int(r['base'], 16)
    frames = r['frames'].split()
    if r['op'] == 'R':
        kind = KIND.get(int(r['type'], 16) & 0x3000, 'other')
        live[base] = (t, int(r['size_kb']), kind, signature(frames, a.frames))
        if a.t0 <= t <= a.t1: made[(kind, live[base][3])] += int(r['size_kb'])
    elif base in live:
        t_r, kb, kind, sig = live.pop(base)
        if a.t0 <= t_r <= a.t1: freed[(kind, sig)] += kb

held = collections.defaultdict(lambda: [0, 0, collections.Counter()])
for base, (t, kb, kind, sig) in live.items():
    if a.t0 <= t <= a.t1:
        h = held[(kind, sig)]
        h[0] += 1; h[1] += kb; h[2][kb] += 1
total = sum(h[1] for h in held.values())
print('Reserved in [%g, %g] s and still held: %.1f MB in %d regions' %
      (a.t0, min(a.t1, 1e9), total / 1024, sum(h[0] for h in held.values())))
print('%8s %5s %9s %9s  %-8s %s' % ('held_MB', 'n', 'made_MB', 'freed_MB', 'kind', 'first non-system frames / sizes KB'))
for key, h in sorted(held.items(), key=lambda kv: -kv[1][1]):
    sizes = ', '.join('%dx%d' % (c, kb) for kb, c in h[2].most_common(4))
    print('%8.1f %5d %9.1f %9.1f  %-8s %s' % (h[1] / 1024, h[0], made[key] / 1024, freed[key] / 1024, key[0], key[1]))
    print('%33s  sizes: %s' % ('', sizes))
churn = [(k, made[k]) for k in made if k not in held and made[k] >= 64 * 1024]
for k, kb in sorted(churn, key=lambda x: -x[1])[:10]:
    print('churn only: %.1f MB made and all freed  %-8s %s' % (kb / 1024, k[0], k[1]))
