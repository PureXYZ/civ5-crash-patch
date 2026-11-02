# Finds the addresses the probe uses in the DX11 exe in another build of the game
# (CivilizationV.exe = DirectX 9, CivilizationV_Tablet.exe), for the tables in exes.c.
# Code: the DX11 instructions at (and before) the address, with relocated dwords and branch
# displacements masked, searched in the other exe's .text. Data: the dword that each matched
# DX11 instruction referencing it holds in the other exe (a vote over up to 12 references).
# llvm-objdump (LLVM-MinGW) decodes the DX11 instructions.
# Usage: python -I portmap.py <dx11 exe> <other exe> [--objdump PATH] [--only NAME,...] [--extra 0xRVA[:data],...]
import argparse, re, struct, subprocess, collections, shutil

# name, kind, DX11 RVA. 'code' = an instruction start (function, call site, return address).
CATALOG = [
    # cmdstream.c
    ('streamCtor', 'code', 0x4ce150),
    ('ctorRetCmds', 'code', 0x4ce19a), ('ctorRetGroups', 'code', 0x4ce1c5), ('ctorRetData', 'code', 0x4ce1e5),
    ('streamSplit', 'code', 0x4ce1e8), ('streamHalves', 'code', 0x4ce20a),
    ('streamBump', 'code', 0x4cfdc0),
    ('boundCmd0c', 'code', 0x4ccdf9), ('boundCmd10', 'code', 0x4cce7b), ('boundCmdList', 'code', 0x4ccefd),
    ('boundGroupEnd', 'code', 0x4cdb30), ('boundGroupEsi28', 'code', 0x4cdc36), ('boundGroup08a', 'code', 0x4cdc9e),
    ('boundGroup0c', 'code', 0x4cdd0e), ('boundGroup08b', 'code', 0x4cdd6e),
    ('flipCall', 'code', 0x4bd9a7), ('flip', 'code', 0x4cdd80),
    ('spareCaller1', 'code', 0x1c9940), ('spareCaller2', 'code', 0x1c9962), ('spareCaller3', 'code', 0x1c9969),
    ('pairPush1', 'code', 0x1c992a), ('pairPush2', 'code', 0x1c9942),
    # msgqueue.c
    ('queueObj', 'data', 0x160e400), ('queuePass', 'code', 0x59d20), ('queueFlip', 'code', 0x90df0),
    ('queuePost', 'code', 0xc230), ('queueWalk', 'code', 0x2a5992), ('queueMainSite', 'code', 0xb4062),
    # uiqueue.c
    ('uiProcess', 'code', 0x385af0), ('uiOwner', 'data', 0x2610200), ('uiLocked', 'data', 0x29e0447),
    ('uiGame', 'data', 0x8c8e74), ('uiMode', 'data', 0x2610f80), ('uiSite', 'code', 0xb3fe1),
    ('uiRet1', 'code', 0x41db0d), ('uiRet2', 'code', 0x41dc8d), ('uiDispatch', 'code', 0x23aba0),
    ('uiListeners', 'code', 0x23e530), ('uiTableArg', 'code', 0x385b8d), ('uiDispatchCall', 'code', 0x385ba2),
    ('uiRegDirty', 'code', 0xc583c), ('uiRegTeamMet', 'code', 0x4a4ec),
    # memwarn.c
    ('uiRegAlert', 'code', 0x24c94c), ('uiAlertCall', 'code', 0x2bd9b9),
    # mapgrid.c
    ('gridReserve', 'code', 0x76018), ('gridReserveFn', 'code', 0x24c620), ('grids', 'data', 0x991270),
    ('gridFree', 'code', 0x413cf0), ('fsbaDealloc', 'code', 0x4138b0),
    # luaheap.c
    ('luaHeapVar', 'data', 0x894b1c), ('luaAlloc', 'code', 0x2ce440),
    # coretick.c
    ('coreSite', 'code', 0x3258b3), ('coreAcc', 'data', 0x8cf91c),
    # luaguard.c
    ('luaLock', 'code', 0x5042a0), ('luaUnlock', 'code', 0x5042b0),
    # probe.c: the game's own NULL write at every start
    ('gameFault', 'code', 0x523dcb),
    # d3dstats.c (diagnostic; D3D11 exes only)
    ('d3dContext', 'data', 0x2c10584), ('d3dFlushUse', 'code', 0x4c12fb), ('d3dFlushVB', 'code', 0x4c1319),
    ('d3dFlushIB', 'code', 0x4c13d8),
    # fluidpool.c
    ('fluidFog', 'data', 0x9a94d8), ('fluidInit', 'code', 0x28f780), ('fluidSeed', 'code', 0x17eeb0),
    ('fluidReset', 'code', 0x109b2a), ('fluidStepCall', 'code', 0x109c5d), ('fluidStep', 'code', 0x2ad1f0),
    ('fluidJob', 'code', 0x1eb5b0), ('fluidStepFn', 'code', 0x2b9f60), ('fluidPassFn', 'code', 0x381a20),
    ('fluidMerge', 'code', 0xd07d0), ('fluidDelete', 'code', 0x413d10), ('fluidFree', 'code', 0x3658c3),
]


class Image:
    def __init__(self, path):
        self.path = path
        d = open(path, 'rb').read()
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        nsec = struct.unpack_from('<H', d, pe + 6)[0]
        self.timestamp = struct.unpack_from('<I', d, pe + 8)[0]
        optsz = struct.unpack_from('<H', d, pe + 20)[0]
        opt = pe + 24
        self.base = struct.unpack_from('<I', d, opt + 28)[0]
        self.size = struct.unpack_from('<I', d, opt + 56)[0]
        dirs = opt + 96
        self.img = bytearray(self.size)
        self.img[:0x1000] = d[:0x1000]
        self.sections = {}
        s = opt + optsz
        for _ in range(nsec):
            name = d[s:s + 8].rstrip(b'\0').decode()
            vsize, va, rsize, raw = struct.unpack_from('<IIII', d, s + 8)
            n = min(rsize, vsize)
            self.img[va:va + n] = d[raw:raw + n]
            self.sections[name] = (va, vsize)
            s += 40
        rva, sz = struct.unpack_from('<II', d, dirs + 5 * 8)
        self.relocs = set()
        p = rva
        while p < rva + sz:
            page, bsz = struct.unpack_from('<II', self.img, p)
            if not bsz:
                break
            for i in range((bsz - 8) // 2):
                e = struct.unpack_from('<H', self.img, p + 8 + 2 * i)[0]
                if e >> 12 == 3:
                    self.relocs.add(page + (e & 0xfff))
            p += bsz
        self.text = self.sections['.text']

    def dword(self, rva):
        return struct.unpack_from('<I', self.img, rva)[0]


BRANCH = re.compile(r'^(call|jmp|j[a-z]+|loop[a-z]*)$')


def decode(objdump, im, start, stop):
    """[(rva, bytes, mnemonic, operands)] for [start, stop)."""
    out = subprocess.run([objdump, '-d', '-M', 'intel', '--start-address=0x%x' % (im.base + start), '--stop-address=0x%x' % (im.base + stop), im.path],
                         capture_output=True, text=True).stdout
    insns = []
    for line in out.splitlines():
        m = re.match(r'^\s+([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(\S*)\s*(.*)$', line)
        if not m:
            continue
        va = int(m.group(1), 16)
        b = bytes.fromhex(m.group(2).replace(' ', ''))
        insns.append((va - im.base, b, m.group(3), m.group(4)))
    return insns


def masked(im, insn):
    """Pattern bytes for one instruction: None where relocated or a branch displacement."""
    rva, b, mn, ops = insn
    pat = list(b)
    for i in range(len(b)):
        for k in range(4):
            if rva + i - k in im.relocs and i - k >= 0:
                pat[i] = None
    if BRANCH.match(mn) and re.match(r'^0x[0-9a-f]+', ops):
        n = 1 if b[0] in (0xEB,) or 0x70 <= b[0] <= 0x7F or 0xE0 <= b[0] <= 0xE3 else 4
        for i in range(len(b) - n, len(b)):
            pat[i] = None
    return pat


def regex(pat):
    return re.compile(b''.join(b'.' if c is None else re.escape(bytes([c])) for c in pat), re.DOTALL)


def find(other, pat):
    va, sz = other.text
    seg = bytes(other.img[va:va + sz])
    return [va + m.start() for m in regex(pat).finditer(seg)]


LENGTHS = (16, 24, 40, 64, 96, 160)


def fwd_candidates(objdump, im, other, rva):
    insns = [i for i in decode(objdump, im, rva, rva + 0x120)]
    if not insns or insns[0][0] != rva:
        return None, 0, []
    best, bestLen, many = None, 0, []
    for L in LENGTHS:
        pat, n = [], 0
        for ins in insns:
            if ins[2] == 'int3' or n >= L:
                break
            pat += masked(im, ins)
            n += len(ins[1])
        if n < min(L, 8):
            break
        m = find(other, pat)
        if not m:
            break
        if len(m) == 1:
            best, bestLen = m[0], n
        else:
            many = m
        if n < L:
            break  # the function ended
    return best, bestLen, many


def back_candidates(objdump, im, other, rva):
    insns = decode(objdump, im, rva - 0xa0, rva)
    insns = [i for i in insns if i[0] + len(i[1]) <= rva]
    if not insns or insns[-1][0] + len(insns[-1][1]) != rva:
        return None, 0, []
    insns = insns[4:]  # the first few may be decoded out of step
    best, bestLen, many = None, 0, []
    for L in LENGTHS:
        pat, n = [], 0
        for ins in reversed(insns):
            if ins[2] == 'int3' or n >= L:
                break
            pat = masked(im, ins) + pat
            n += len(ins[1])
        if n < min(L, 8):
            break
        m = find(other, pat)
        if not m:
            break
        if len(m) == 1:
            best, bestLen = m[0] + n, n
        else:
            many = [x + n for x in m]
        if n < L:
            break
    return best, bestLen, many


def map_code(objdump, im, other, rva, near=None):
    """near(rva) -> the other exe's shift (other - DX11) at the closest address matched uniquely,
    to choose among several matches of a short function."""
    f, fl, fm = fwd_candidates(objdump, im, other, rva)
    b, bl, bm = back_candidates(objdump, im, other, rva)
    if f is not None and b is not None:
        return (f, 'ok (%d+%d bytes)' % (bl, fl)) if f == b else (None, 'CONFLICT fwd 0x%x back 0x%x' % (f, b))
    if f is not None:
        return f, 'forward only (%d bytes)' % fl
    if b is not None:
        return b, 'backward only (%d bytes)' % bl
    many = sorted(set(fm) | set(bm))
    if many and near:
        shift, at = near(rva)
        if shift is not None:
            want = rva + shift
            c = min(many, key=lambda x: abs(x - want))
            if abs(c - want) <= 0x400:
                return c, 'one of %d matches, %+d bytes from the shift 0x%x of exe+0x%x' % (len(many), c - want, shift & 0xffffffff, at)
    return None, 'NOT FOUND (%d matches)' % len(many)


def map_data(objdump, im, other, rva):
    va = im.base + rva
    refs = sorted(r for r in im.relocs if im.dword(r) == va and im.text[0] <= r < im.text[0] + im.text[1])
    votes = collections.Counter()
    tried = 0
    for ref in refs[:12]:
        insns = decode(objdump, im, ref - 0x30, ref + 0x10)
        hit = [k for k, i in enumerate(insns) if i[0] <= ref and ref + 4 <= i[0] + len(i[1])]
        if not hit or hit[0] < 3:
            continue
        ins = insns[hit[0]]
        tried += 1
        p, how = map_code(objdump, im, other, ins[0])
        if p is None:
            continue
        at = p + (ref - ins[0])
        if at not in other.relocs:
            continue
        votes[other.dword(at) - other.base] += 1
    if not votes:
        return None, 'NOT FOUND (%d references, %d tried)' % (len(refs), tried)
    (v, n), = votes.most_common(1)
    note = '%d of %d references agree' % (n, tried)
    if len(votes) > 1:
        note += '; DISAGREE: ' + ', '.join('0x%x x%d' % kv for kv in votes.items())
    return v, note


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dx11')
    ap.add_argument('other')
    ap.add_argument('--objdump', default=shutil.which('llvm-objdump') or 'llvm-objdump')
    ap.add_argument('--only', default='')
    ap.add_argument('--extra', default='', help='more DX11 RVAs: 0xRVA (code) or 0xRVA:data, comma-separated')
    a = ap.parse_args()
    im, other = Image(a.dx11), Image(a.other)
    print('# %s (timestamp 0x%08x, SizeOfImage 0x%x) from %s (0x%08x)' % (a.other, other.timestamp, other.size, a.dx11,
                                                                         im.timestamp))
    cat = [c for c in CATALOG if not a.only or c[0] in a.only.split(',')]
    for e in filter(None, a.extra.split(',')):
        r, _, k = e.partition(':')
        cat.append(('extra_' + r, k or 'code', int(r, 16)))
    found, rows = {}, []
    for name, kind, rva in cat:
        v, note = (map_data if kind == 'data' else map_code)(a.objdump, im, other, rva)
        rows.append([name, kind, rva, v, note])
        if v is not None and kind == 'code' and not note.startswith('one of'):
            found[rva] = v - rva

    def near(rva):
        if not found:
            return None, None
        at = min(found, key=lambda r: abs(r - rva))
        return found[at], at
    for row in rows:  # second pass: short functions found many times
        if row[3] is None and row[1] == 'code':
            row[3], row[4] = map_code(a.objdump, im, other, row[2], near)
    for name, kind, rva, v, note in rows:
        print('%-16s %-4s 0x%07x -> %-10s %s' % (name, kind, rva, '0x%x' % v if v is not None else '-', note))


if __name__ == '__main__':
    main()
