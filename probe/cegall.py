# Which code Steam's CEG checksums in a game exe, and whether given places are inside it.
# The exe has `mov eax, fn; call eax; ret` stubs (26 in the DX11 exe) whose functions CRC a table of
# code ranges (relative pointers, sizes, the ranges' relocations) and end in `not eax`. The game uses
# the results: changing a byte inside one range of exe+0x2d00f0 made per-thread arrays at map load
# far too large to fit (crash), and the main loop's (exe+0x3258b3) made the game quit. Keep every
# code change outside these ranges. llvm-objdump (LLVM-MinGW) decodes the checks' loops.
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
t_va, t_size, t_raw = text[2], max(text[1], text[3]), text[4]

def off(rva):
    for name, vs, va, rs, raw in secs:
        if va <= rva < va + max(vs, rs): return raw + rva - va
    raise ValueError(hex(rva))
def s32(rva): return struct.unpack_from('<i', data, off(rva))[0]
def u32(rva): return struct.unpack_from('<I', data, off(rva))[0]

# The stubs: B8 <fn> FF D0 C3 with fn in .text.
stubs = []
blob = data[t_raw:t_raw + t_size]
for m in re.finditer(rb'\xB8(....)\xFF\xD0\xC3', blob, re.S):
    fn = struct.unpack('<I', m.group(1))[0] - base
    if t_va <= fn < t_va + t_size: stubs.append((t_va + m.start(), fn))

def decode(rva, n):
    out = subprocess.run([a.objdump, '-d', '--no-show-raw-insn', '--start-address=0x%x' % (base + rva),
                          '--stop-address=0x%x' % (base + rva + n), a.exe], capture_output=True, text=True).stdout
    return [re.sub(r'\s+', ' ', l.split(':', 1)[1].split(' #')[0]).strip()
            for l in out.splitlines() if re.match(r'^\s*[0-9a-f]+:\s', l)]

ranges = []
for stub, fn in stubs:
    body = decode(fn, 0x100)
    t1 = td = cnt = None
    for k, t in enumerate(body):
        m = re.match(r'leal 0x([0-9a-f]+)\(%eax,%esi\), %ecx', t)
        if m and t1 is None: t1 = int(m.group(1), 16) - base
        m = re.match(r'movl 0x([0-9a-f]+)\(%esi\), %eax', t)
        if m and body[k + 1:k + 3] == ['pushl %eax', 'pushl %ecx']: td = int(m.group(1), 16) - base
        m = re.match(r'cmpl \$0x([0-9a-f]+), %esi', t)
        if m and cnt is None: cnt = int(m.group(1), 16) // 4
    if None in (t1, td, cnt):
        print('stub exe+0x%x -> exe+0x%x: not the known loop, skipped' % (stub, fn))
        continue
    for j in range(cnt):
        p = t1 + 4 * j + s32(t1 + 4 * j)
        ranges.append((p, p + u32(td + 4 * j), stub, j))
ranges.sort()
print('%d checks, %d ranges, %.1f MB of code (overlaps counted), exe+0x%x..0x%x' % (
    len(stubs), len(ranges), sum(e - s for s, e, _, _ in ranges) / 1048576, ranges[0][0], max(e for _, e, _, _ in ranges)))
if a.list:
    for s, e, st, j in ranges: print('exe+0x%x..0x%x  check exe+0x%x #%d' % (s, e, st, j))
bad = 0
for site in a.sites:
    r, _, n = site.partition(':')
    r, n = int(r, 16), int(n) if n else 6
    hit = sorted({'exe+0x%x' % st for s, e, st, _ in ranges if s < r + n and r < e})
    bad += bool(hit)
    print('exe+0x%x: %s' % (r, 'inside the ranges of ' + ', '.join(hit) if hit else 'outside every range'))
if a.sites: print('%d of %d places inside a checked range' % (bad, len(a.sites)))
