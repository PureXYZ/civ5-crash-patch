# Watches the Community Patch DLL (github.com/LoneGazebo/Community-Patch-DLL) for changes that
# could meet this patch in the game's process. That DLL has its own table of exe addresses
# (CvGameCoreDLL_Expansion2/Exe/CvExeAddresses.h): it checks signature bytes and cross-references
# there before it uses them, and it may read or write the same objects this patch manages.
#
# What it compares, per exe:
#   - their signature and check bytes against the code this patch rewrites (exes.c and the
#     lengths in cmdstream.c). A hit means their check fails once the patch is loaded, and by
#     their rules every symbol that depends on it is dropped: that feature of theirs stays off.
#   - their data addresses against the objects this patch manages (queues, tables, pools).
#   - their code addresses against the code this patch reads or calls (same function = same job).
# It also fingerprints their Exe folder, and looks at every open pull request and every one merged
# since the last review for changes to that folder or new uses of low-level calls (page
# protection, heaps, import tables, exception filters, threads).
#
# The result for their main branch must equal vpcompat.txt, and every such pull request must be
# marked reviewed there. Anything else exits 1 with what changed: review it against
# docs/HOW-IT-WORKS.md ("Living with other DLLs"), then record it with --update.
#
# Usage: python -I vpcompat.py                 check (needs the GitHub CLI, gh)
#        python -I vpcompat.py --update        record the current state as reviewed
#        python -I vpcompat.py --ref REF       only print the comparison for a branch, tag or commit
#        python -I vpcompat.py --file PATH     the same for a local copy of their address table
# Exit: 0 nothing new, 1 something to review, 2 could not run (also: a field of exes.c that
# FIELDS below doesn't classify yet).
import argparse, datetime, difflib, hashlib, json, os, re, subprocess, sys

REPO = 'LoneGazebo/Community-Patch-DLL'
EXE_DIR = 'CvGameCoreDLL_Expansion2/Exe'
TABLE = EXE_DIR + '/CvExeAddresses.h'
HERE = os.path.dirname(os.path.abspath(__file__))
BASELINE = os.path.join(HERE, 'vpcompat.txt')
BASE = 0x400000              # their table holds addresses at the preferred base; exes.c holds offsets
COLUMNS = ['DX11', 'DX9', 'tablet']  # their column order, by this patch's exe names
NEAR = 0x40                  # code this close to one of the patch's places counts as the same code
CHECK_LEN = {'EXE_ABS32': 4, 'EXE_REL32': 5, 'EXE_VAL8': 1, 'EXE_VAL32': 4}
CODE_KINDS, DATA_KINDS = ('EXE_FUNCTION', 'EXE_CODE'), ('EXE_DATA', 'EXE_VTABLE')
SOURCE = ('.cpp', '.h', '.hpp', '.inl', '.c')
LOW_LEVEL = re.compile(r'\b(VirtualProtect\w*|VirtualAlloc\w*|VirtualFree\w*|WriteProcessMemory|FlushInstructionCache|'
                       r'HeapCreate|HeapAlloc|HeapFree|IMAGE_IMPORT_DESCRIPTOR|IMAGE_THUNK_DATA\w*|'
                       r'SetUnhandledExceptionFilter|AddVectoredExceptionHandler|CreateThread|_beginthread\w*|'
                       r'lua_newstate|lua_setallocf|sqlite3_config|_set_new_handler|_msize)\b')


def die(msg):
    print('vpcompat: ' + msg, file=sys.stderr)
    sys.exit(2)


def strip_comments(s):
    return re.sub(r'//[^\n]*', '', re.sub(r'/\*.*?\*/', '', s, flags=re.S))


# ---- This patch: what it writes, reads and manages in each exe ------------------------------

def patch_fields():
    """exes.c field -> (kind, source file, ...). Kinds: x not an address; c code the patch reads,
    calls or compares with; w code it rewrites (length or lengths, setting, on by default, offset);
    d data it manages (size). A new field in exes.c must be added here."""
    cs = open(os.path.join(HERE, 'cmdstream.c'), encoding='utf-8').read()
    one = lambda pat: int(re.search(pat, cs).group(1))
    try:
        split, halves, list1 = one(r'kSplit\[(\d+)\]'), one(r'kHalves\[(\d+)\]'), one(r'kBound1List = \{(\d+),')
        bounds = [int(n) for n in re.findall(r'\n\s*\{(\d+), \{0x', re.search(r'kBounds\[8\] = \{(.*?)\n\};', cs, re.S).group(1))]
    except AttributeError:
        die('cmdstream.c: the stub lengths (kSplit, kHalves, kBounds, kBound1List) are not where they were')
    if len(bounds) != 8: die('cmdstream.c: kBounds has %d lengths, not 8' % len(bounds))
    f = {}

    def put(kind, src, names, *more):
        for n in names.split(): f[n] = (kind, src) + more
    put('x', '', 'name timestamp imageSize streamHalves recordRound texPoolFixups texRecFixups texSiteCount')
    put('c', 'probe.c', 'gameFault')
    put('c', 'cmdstream.c', 'streamCtor ctorRets flip spareCallers')
    put('w', 'cmdstream.c', 'streamSplit', split, 'CommandStreamSplit', True, 0)
    put('w', 'cmdstream.c', 'streamHalvesSite', halves, 'CommandStreamSplit', True, 0)
    put('w', 'cmdstream.c', 'streamBump', 5, 'CommandStreamCap', True, 0)
    put('w', 'cmdstream.c', 'bounds', (bounds, list1), 'CommandStreamCap', True, 0)
    put('w', 'cmdstream.c', 'flipCall streamReset', 5, 'CommandStreamStats', True, 0)
    put('w', 'cmdstream.c', 'pairPush', 4, 'CommandStreamMB', False, 1)
    put('c', 'msgqueue.c', 'queuePass queueFlip queuePost queueWalk queueMainSite')
    put('d', 'msgqueue.c', 'queueObj', 0x1001004)        # two queues and their lock
    put('c', 'uiqueue.c', 'uiProcess uiSite uiRet1 uiRet2 uiDispatch uiListeners uiRegDirty uiRegTeamMet')
    put('c', 'memwarn.c', 'uiRegAlert uiAlertCall')
    put('d', 'uiqueue.c', 'uiOwner', 8 + 2 * 0x600)      # the index and both handler tables
    put('d', 'uiqueue.c', 'uiGame uiMode', 4)
    put('d', 'uiqueue.c', 'uiLocked', 1)
    put('c', 'mapgrid.c', 'gridReserve gridReserveFn gridFree')
    put('d', 'mapgrid.c', 'grids', 0x100)
    put('c', 'luaheap.c', 'luaAlloc')
    put('d', 'luaheap.c', 'luaHeapVar', 4)
    put('c', 'coretick.c', 'coreSite')
    put('d', 'coretick.c', 'coreAcc', 4)
    put('c', 'luaguard.c', 'luaLock luaUnlock')
    put('c', 'd3dstats.c', 'd3dFlushUse d3dFlushVB d3dFlushIB')
    put('d', 'd3dstats.c', 'd3dContext', 4)
    put('d', 'texhandles.c', 'texPool', 0x328c0 + 0x3000 * 0x398)  # the pool and its records
    put('d', 'texhandles.c', 'texRecords', 0x3000 * 0x398)
    put('w', 'texhandles.c', 'texSites', 4, 'TextureHandles', True, 0)
    put('c', 'terrain.c', 'terrainPoolCall terrainInit terrainSort terrainLists terrainMgrLoad terrainAlloc')
    put('w', 'terrain.c', 'terrainHandCount terrainHandArray terrainRelease', 6, 'TerrainBaseSlots', True, 0)
    put('d', 'terrain.c', 'terrainObj', 0x76000)
    put('c', 'intro.c', 'introCreate introSkipCopy')
    put('d', 'intro.c', 'skipIntro', 1)
    put('c', 'fluidpool.c', 'fluidInit fluidReset fluidJob fluidStepFn fluidPassFn fluidMerge fluidFree')
    put('w', 'fluidpool.c', 'fluidSeed', 6, 'FluidPools', True, 0)
    put('w', 'fluidpool.c', 'fluidStepCall', 5, 'FluidPools', True, 0)
    return f


def patch_exes():
    """exe name -> writes [(rva, len, place, setting, default on)], code [(rva, place)],
    data [(rva, size, place)] from exes.c."""
    src = strip_comments(open(os.path.join(HERE, 'exes.c'), encoding='utf-8').read())
    fields = patch_fields()
    arrays = {m.group(1): [int(t, 16) for t in re.findall(r'\{(0x[0-9a-fA-F]+),', m.group(2))]
              for m in re.finditer(r'static const TexSite (\w+)\[\] = \{(.*?)\n\};', src, re.S)}
    body = re.search(r'static const GameExe kExes\[\] = \{(.*?)\n\};', src, re.S)
    if not body: die('exes.c: kExes not found')
    blocks = re.split(r'(?=\.name\s*=\s*")', body.group(1))[1:]
    exes = {}
    for b in blocks:
        vals = {}
        for m in re.finditer(r'\.(\w+)\s*=\s*(\{[^{}]*\}|"[^"]*"|[^,{}]+)', b):
            k, v = m.group(1), m.group(2).strip()
            if k not in fields: die('exes.c field "%s" is not classified in patch_fields()' % k)
            if v.startswith('{'): vals[k] = [int(t, 0) for t in re.findall(r'0x[0-9a-fA-F]+|\d+', v)]
            elif v.startswith('"'): vals[k] = v.strip('"')
            elif re.fullmatch(r'0x[0-9a-fA-F]+|\d+', v): vals[k] = int(v, 0)
            else: vals[k] = v  # an array's name or a sizeof
        e = {'writes': [], 'code': [], 'data': []}
        for k, v in vals.items():
            kind = fields[k][0]
            if kind == 'x' or v == 0: continue
            src_file = fields[k][1]
            if k == 'texSites':
                if v not in arrays: die('exes.c: array %s not found' % v)
                v = arrays[v]
            items = v if isinstance(v, list) else [v]
            for i, rva in enumerate(items):
                place = '%s%s (%s)' % (k, '[%d]' % i if isinstance(v, list) else '', src_file)
                if kind == 'd': e['data'].append((rva, fields[k][2], place))
                else: e['code'].append((rva, place))
                if kind == 'w':
                    _, _, length, setting, on, off = fields[k]
                    if k == 'bounds': length = length[1] if i == 2 and vals.get('streamHalves') == 1 else length[0][i]
                    e['writes'].append((rva + off, length, place, setting, on))
        exes[vals['name']] = e
    if sorted(exes) != sorted(COLUMNS): die('exes.c: exes %s, expected %s' % (sorted(exes), sorted(COLUMNS)))
    return exes


# ---- Their address table --------------------------------------------------------------------

def parse_table(text):
    src = strip_comments(text)
    syms, checks, notes = [], [], []
    for m in re.finditer(r'\bEXE_SYMBOL\s*\(\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*((?:"[^"]*"\s*)+|NULL)\s*\)', src):
        name, kind = m.group(1), m.group(2)
        try:
            vals = [None if v == 'EXE_NONE' else int(v, 16) for v in m.group(3, 4, 5)]
            sig = None if m.group(6).strip() == 'NULL' else [None if t == '??' else int(t, 16)
                                                             for t in ''.join(re.findall(r'"([^"]*)"', m.group(6))).split()]
        except ValueError:
            notes.append('entry %s is not understood' % name)
            continue
        if kind not in CODE_KINDS + DATA_KINDS + ('EXE_OFFSET',): notes.append('%s has the unknown kind %s' % (name, kind))
        syms.append({'name': name, 'kind': kind, 'vals': vals, 'sig': sig})
    names = {s['name'] for s in syms}
    for m in re.finditer(r'\bEXE_CHECK\s*\(\s*(\w+)\s*,\s*(\w+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*(\w+)\s*\)', src):
        kind, frm, off, to = m.group(1), m.group(2), int(m.group(3), 0), m.group(4)
        if kind not in CHECK_LEN or frm not in names or to not in names:
            notes.append('check %s %s+0x%X -> %s is not understood' % (kind, frm, off, to))
            continue
        checks.append((kind, frm, off, to))
    for macro, got in (('EXE_SYMBOL', len(syms)), ('EXE_CHECK', len(checks))):
        n = len(re.findall(r'\b%s\s*\(' % macro, src))
        if n != got: notes.append('%d of %d %s entries are not understood' % (n - got, n, macro))
    for other in sorted(set(re.findall(r'\b(EXE_[A-Z0-9_]+)\s*\(', src)) - {'EXE_SYMBOL', 'EXE_CHECK'}):
        notes.append('new kind of entry: %s' % other)
    return syms, checks, notes


def compare(syms, checks, patch, col):
    """The lines for one exe: what they share with the patch there, and what the patch's writes
    turn off by their own rules (signature, checks, then trust spreading; CvExeSymbols.cpp)."""
    out = []
    by = {s['name']: s for s in syms}
    addr = lambda s: None if s['vals'][col] is None or s['kind'] == 'EXE_OFFSET' else s['vals'][col] - BASE
    ok, why = {}, {}

    def hits(rva, length, fixed=None):
        found = []
        for w_rva, w_len, place, setting, on in patch['writes']:
            lo, hi = max(rva, w_rva), min(rva + length, w_rva + w_len)
            if lo < hi and (fixed is None or any(fixed[i - rva] is not None for i in range(lo, hi))):
                found.append((place, setting, on))
        return found

    def note(what, found):
        fail = False
        for place, setting, on in found:
            out.append('  write %s%s rewrites %s: %s' % (setting, ' (on by default)' if on else ' (off by default)', place, what))
            fail = fail or on
        return fail

    for s in syms:
        a = addr(s)
        ok[s['name']] = s['vals'][col] is not None
        if a is None: continue
        if s['kind'] in DATA_KINDS:
            for rva, size, place in patch['data']:
                if rva <= a < rva + size: out.append('  data  %s = %s + 0x%x' % (s['name'], place, a - rva))
        else:
            n = len(s['sig']) if s['sig'] else 1
            for rva, place in patch['code']:
                if a - NEAR <= rva < a + n + NEAR: out.append('  code  %s %+#x = %s' % (s['name'], rva - a, place))
            if s['sig'] and note('the signature of %s fails' % s['name'], hits(a, n, s['sig'])): ok[s['name']] = False
    passed = []
    for kind, frm, off, to in checks:
        a = addr(by[frm])
        if a is None or by[to]['vals'][col] is None:
            passed.append(False)
            continue
        good = not note('the check %s+0x%X -> %s fails' % (frm, off, to), hits(a + off, CHECK_LEN[kind]))
        passed.append(good)
        if not good: ok[frm] = ok[to] = False
    changed = True
    while changed:
        changed = False
        vouched = dict.fromkeys(ok, 0)
        for good, (kind, frm, off, to) in zip(passed, checks):
            if not good or frm == to: continue
            if ok[frm] and not ok[to]: ok[frm], changed = False, True
            if ok[frm]: vouched[to] += 1
        for s in syms:
            if ok[s['name']] and s['sig'] is None and not vouched[s['name']]: ok[s['name']], changed = False, True
    off_now = [s['name'] for s in syms if s['vals'][col] is not None and not ok[s['name']]]
    if off_now: out.append('  off   with the patch\'s default settings they drop: ' + ', '.join(off_now))
    return out or ['  nothing shared']


def report(table_text, patch):
    syms, checks, notes = parse_table(table_text)
    out = ['table: %d symbols, %d checks' % (len(syms), len(checks))]
    out += ['  ! ' + n for n in notes]
    for s in syms:
        out.append('  %s %s %s' % (s['name'], s['kind'], ' '.join('-' if v is None else '%08x' % v for v in s['vals'])))
    for col, exe in enumerate(COLUMNS):
        out += ['%s: %s' % (exe, l.strip()) for l in compare(syms, checks, patch[exe], col)]
    return out


# ---- Their repository -----------------------------------------------------------------------

def gh(*args):
    try:
        r = subprocess.run(['gh'] + list(args), capture_output=True, text=True, encoding='utf-8', errors='replace')
    except FileNotFoundError:
        die('the GitHub CLI (gh) is not installed')
    if r.returncode: raise RuntimeError('gh %s: %s' % (' '.join(args[:3]), r.stderr.strip()[:300]))
    return r.stdout


def fetch(path, ref):
    return gh('api', 'repos/%s/contents/%s?ref=%s' % (REPO, path, ref), '-H', 'Accept: application/vnd.github.raw')


def low_level(lines):
    return sorted({'%s: %s' % (path, m.group(1)) for path, line in lines for m in LOW_LEVEL.finditer(line)})


def main_branch(patch):
    """Their Exe folder on the main branch: its fingerprint, the comparison, the low-level calls."""
    files = sorted((f['name'], f['sha']) for f in json.loads(gh('api', 'repos/%s/contents/%s?ref=master' % (REPO, EXE_DIR)))
                   if f['type'] == 'file')
    out = ['folder: %d files, %s' % (len(files), hashlib.sha1(repr(files).encode()).hexdigest()[:12])]
    text = {name: fetch(EXE_DIR + '/' + name, 'master') for name, _ in files}
    if TABLE.split('/')[-1] not in text: return out + ['  ! the address table is gone'], []
    lines = report(text[TABLE.split('/')[-1]], patch)
    calls = low_level([(name, l) for name, t in text.items() for l in strip_comments(t).splitlines()])
    return out + lines + ['low-level calls in the folder: ' + ('; '.join(calls) or 'none')], lines


def changes(since, patch, main_lines, reviewed):
    """Open pull requests, and those merged since the last review, that change the Exe folder or
    add low-level calls. -> (fingerprints seen, text for the ones not reviewed yet)."""
    fields = 'number,title,headRefOid,files,state'
    prs = json.loads(gh('pr', 'list', '-R', REPO, '--state', 'open', '--limit', '300', '--json', fields))
    prs += json.loads(gh('pr', 'list', '-R', REPO, '--state', 'merged', '--limit', '300', '--search', 'merged:>=' + since, '--json', fields))
    seen, new = [], []
    for pr in sorted(prs, key=lambda p: p['number']):
        paths = [f['path'] for f in pr['files']]
        if len(paths) < 100 and not any(p.startswith(EXE_DIR + '/') or p.endswith(SOURCE) for p in paths): continue
        try:
            diff = gh('pr', 'diff', str(pr['number']), '-R', REPO)
        except RuntimeError as e:  # too large to serve: decide by the file list
            if not any(p.startswith(EXE_DIR + '/') or p.endswith(SOURCE) for p in paths): continue
            diff, exe, calls = None, ['unread ' + pr['headRefOid']], ['the diff could not be read (%s)' % str(e)[-80:]]
        if diff is not None:
            path, exe, added = '', [], []
            for line in diff.splitlines():
                if line.startswith('+++ ') or line.startswith('--- '):
                    if line[4:] != '/dev/null': path = line[6:].rstrip('\t')
                elif line[:1] in '+-' and path.startswith(EXE_DIR + '/'): exe.append(path + line)
                if line[:1] == '+' and not line.startswith('+++') and path.endswith(SOURCE): added.append((path, line))
            calls = low_level(added)
        if not exe and not calls: continue
        fp = hashlib.sha1('\n'.join(exe + calls).encode()).hexdigest()[:12]
        seen.append(fp)
        if fp in reviewed: continue
        out = ['%s pull request %d "%s" (%s), fingerprint %s' % (pr['state'].lower(), pr['number'], pr['title'], pr['headRefOid'][:8], fp)]
        out += ['  low-level call added: ' + c for c in calls]
        if exe:
            out.append('  changes %d lines in %s' % (len(exe), EXE_DIR))
            try:
                theirs = report(fetch(TABLE, pr['headRefOid']), patch)
                out += ['  ' + l for l in difflib.unified_diff(main_lines, theirs, 'main branch', 'pull request', lineterm='', n=0)
                        if not l.startswith(('---', '+++', '@@'))] or ['  the same table as the main branch']
            except RuntimeError as e:
                out.append('  its address table could not be read: %s' % e)
        new.append('\n'.join(out))
    return seen, new


def load_baseline():
    since, reviewed, lines = None, set(), []
    if os.path.exists(BASELINE):
        for l in open(BASELINE, encoding='utf-8').read().splitlines():
            if l.startswith('#'): continue
            m = re.match(r'(checked|reviewed) (\S+)$', l)
            if m and not lines:
                if m.group(1) == 'checked': since = m.group(2)
                else: reviewed.add(m.group(2))
            else: lines.append(l)
    return since, reviewed, lines


def main():
    ap = argparse.ArgumentParser(description='Compare the Community Patch DLL\'s exe interop with this patch.')
    ap.add_argument('--update', action='store_true', help='record the current state as reviewed')
    ap.add_argument('--ref', help='only print the comparison for this branch, tag or commit')
    ap.add_argument('--file', help='only print the comparison for this local copy of their address table')
    a = ap.parse_args()
    patch = patch_exes()
    if a.file or a.ref:
        text = open(a.file, encoding='utf-8').read() if a.file else fetch(TABLE, a.ref)
        print('\n'.join(report(text, patch)))
        return 0
    today = datetime.datetime.now(datetime.timezone.utc).date()
    since, reviewed, old = load_baseline()
    try:
        now, main_lines = main_branch(patch)
        day = datetime.date.fromisoformat(since) if since else today - datetime.timedelta(days=14)
        seen, new = changes((day - datetime.timedelta(days=1)).isoformat(), patch, main_lines, set() if a.update else reviewed)
    except (RuntimeError, ValueError, KeyError) as e:
        die(str(e))
    if a.update:
        head = ['# Written by vpcompat.py --update after a review; see the top of vpcompat.py.',
                '# "reviewed" lines are fingerprints of pull requests there that were looked at.',
                'checked ' + today.isoformat()] + ['reviewed ' + fp for fp in sorted(set(seen))]
        with open(BASELINE, 'w', encoding='utf-8', newline='\n') as f: f.write('\n'.join(head + now) + '\n')
        print('vpcompat: recorded (%d pull requests marked reviewed)' % len(set(seen)))
        return 0
    status = 0
    if now != old:
        status = 1
        print('Their main branch no longer matches vpcompat.txt:')
        print('\n'.join(l for l in difflib.unified_diff(old, now, 'vpcompat.txt', 'now', lineterm='', n=1)))
    if new:
        status = 1
        print('\nNot reviewed yet:\n' + '\n\n'.join(new))
    print('\nvpcompat: review the above, then run vpcompat.py --update' if status else
          'vpcompat: nothing new (main branch as recorded, %d pull requests already reviewed)' % len(seen))
    return status


if __name__ == '__main__':
    sys.exit(main())
