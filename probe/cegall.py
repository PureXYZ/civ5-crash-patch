# Which code Steam's CEG checksums in a game exe, and whether given places are inside it.
# The exe CRCs code with loops over tables of code ranges (self-relative pointers, sizes, the
# ranges' relocations): 284 loops over 142 tables in each of the three exes, ~80% of .text. 26 of
# them sit in functions behind `mov eax, fn; call eax; ret` stubs; the rest are inline in game
# code. The game uses the results: changing a byte inside one range of exe+0x2d00f0 made per-thread
# arrays at map load far too large to fit (crash), and the main loop's (exe+0x3258b3) made the game
# quit. Keep every code change outside these ranges. llvm-objdump (LLVM-MinGW) decodes .text.
# A loop: body from a `jb` target back to `cmpl $N, %esi; jb`, holding a call whose 3rd pushed
# argument (counting back from the call) is `lea T(%r,%esi)` (r = [T+esi], a range start) and whose
# 4th is `mov S(%esi)` (its size).
# Usage: python -I cegall.py <exe> [0xRVA[:len] ...] [--objdump PATH] [--list]
#   RVAs are exe-relative (exe+0x1bd079 -> 0x1bd079); len defaults to 6 (the bytes changed there).
import argparse, re, shutil, struct, subprocess

ap = argparse.ArgumentParser()
ap.add_argument('exe')
ap.add_argument('sites', nargs='*')
ap.add_argument('--objdump', default=shutil.which('llvm-objdump') or 'llvm-objdump')
ap.add_argument('--list', action='store_true', help='print every checked range')
a = ap.parse_args()

data = open(a.exe, 'rb').read()
pe = struct.unpack_from('<I', data, 0x3c)[0]
nsec = struct.unpack_from('<H', data, pe + 6)[0]
base = struct.unpack_from('<I', data, pe + 24 + 28)[0]
so = pe + 24 + struct.unpack_from('<H', data, pe + 20)[0]
secs = [struct.unpack_from('<8sIIII', data, so + 40 * i) for i in range(nsec)]
text = next(s for s in secs if s[0].rstrip(b'\0') == b'.text')
t_va, t_size = text[2], text[1]

def off(rva):
    for name, vs, va, rs, raw in secs:
        if va <= rva < va + max(vs, rs): return raw + rva - va
    raise ValueError(hex(rva))
def s32(rva): return struct.unpack_from('<i', data, off(rva))[0]
def u32(rva): return struct.unpack_from('<I', data, off(rva))[0]

out = subprocess.run([a.objdump, '-d', '--no-show-raw-insn', '--section=.text', a.exe], capture_output=True, text=True).stdout
vas, code, at = [], [], {}
for l in out.splitlines():
    m = re.match(r'^\s*([0-9a-f]+):\s+(.*)$', l)
    if m:
        at[int(m.group(1), 16)] = len(code)
        vas.append(int(m.group(1), 16) - base)
        code.append(re.sub(r'\s+', ' ', m.group(2).split(' #')[0]).strip())

def defined(k, reg, top):
    for j in range(k - 1, top - 1, -1):
        if code[j].endswith(', %' + reg): return code[j]
    return ''

loops = {}  # (range table, size table, count) -> the loops that walk it
for i, t in enumerate(code):
    m = re.match(r'cmpl \$0x([0-9a-f]+), %esi$', t)
    jb = m and i + 1 < len(code) and re.match(r'jb 0x([0-9a-f]+)', code[i + 1])
    if not jb: continue
    top = at.get(int(jb.group(1), 16))
    if top is None or not i - 80 < top < i: continue
    for c in range(top, i):
        if not code[c].startswith('calll'): continue
        pushes = [(k, code[k][6:]) for k in range(c - 1, top - 1, -1) if code[k].startswith('pushl ')]
        if len(pushes) < 4 or not pushes[2][1].startswith('%') or not pushes[3][1].startswith('%'): continue
        t1 = re.match(r'leal 0x([0-9a-f]+)\(%\w+,%esi\), %\w+$', defined(pushes[2][0], pushes[2][1][1:], top))
        td = re.match(r'movl 0x([0-9a-f]+)\(%esi\), %\w+$', defined(pushes[3][0], pushes[3][1][1:], top))
        if t1 and td:
            key = (int(t1.group(1), 16) - base, int(td.group(1), 16) - base, int(m.group(1), 16) // 4)
            loops.setdefault(key, []).append(vas[i])
            break

ranges = []
for (t1, td, cnt), at_loops in loops.items():
    for j in range(cnt):
        p = t1 + 4 * j + s32(t1 + 4 * j)
        for lp in at_loops: ranges.append((p, p + u32(td + 4 * j), lp, j))
ranges.sort()
covered, end = 0, 0
for s, e, _, _ in ranges:
    s, e = max(s, end, t_va), min(e, t_va + t_size)
    if e > s: covered += e - s
    end = max(end, e)
print('%d loops over %d tables, %d ranges, %.2f MB of .text (%.1f%%), exe+0x%x..0x%x' % (
    sum(len(v) for v in loops.values()), len(loops), len(ranges), covered / 1048576, 100 * covered / t_size,
    ranges[0][0], max(e for _, e, _, _ in ranges)))
if a.list:
    for s, e, lp, j in ranges: print('exe+0x%x..0x%x  loop exe+0x%x #%d' % (s, e, lp, j))
bad = 0
for site in a.sites:
    r, _, n = site.partition(':')
    r, n = int(r, 16), int(n) if n else 6
    hit = sorted({lp for s, e, lp, _ in ranges if s < r + n and r < e})
    bad += bool(hit)
    print('exe+0x%x: %s' % (r, 'inside the ranges of %d loops (exe+%s)' % (len(hit), ', exe+'.join('0x%x' % h for h in hit))
                            if hit else 'outside every range'))
if a.sites: print('%d of %d places inside a checked range' % (bad, len(a.sites)))
