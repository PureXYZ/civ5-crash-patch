# Names the sounds in a sounds_<session>.log (SoundLog=1). The game gives Miles only a sound's file
# type (".wav", ".mp3"), so the log has the type and the size in bytes; this matches the size
# against the game's sound files: loose .wav/.mp3/.ogg files under the game folder (and --mods)
# and the entries of its sound packs (*Sound*.fpk). Sizes that several files share print all of
# them (up to 3). Streams are logged under Miles' memory-file names and stay unnamed.
# Prints the most started sounds, the starts per 5 s window, and with --from/--to every start
# in that window.
# Usage: python -I soundnames.py <sounds log> <game folder> [--mods DIR] [--top N] [--from S] [--to S]
import argparse, collections, os, re, struct

SOUND_EXT = ('.wav', '.mp3', '.ogg')


def fpk_entries(path):
    # version, "FPK_", 2 bytes, file count; per file: name length, name (padded to 4 bytes),
    # 8 bytes, size, offset
    data = open(path, 'rb').read()
    if data[4:8] != b'FPK_':
        return
    count = struct.unpack_from('<I', data, 0x0a)[0]
    p = 0x0e
    for _ in range(count):
        n = struct.unpack_from('<I', data, p)[0]
        name = data[p + 4:p + 4 + n].decode('latin-1')
        end = p + 4 + n + (-n % 4)
        yield name, struct.unpack_from('<I', data, end + 8)[0]
        p = end + 16


def index(folders):
    by_size = collections.defaultdict(set)
    for folder in folders:
        for root, _, files in os.walk(folder):
            for f in files:
                low = f.lower()
                path = os.path.join(root, f)
                if low.endswith(SOUND_EXT):
                    by_size[os.path.getsize(path)].add(f)
                elif low.endswith('.fpk') and 'sound' in low:
                    for name, size in fpk_entries(path):
                        by_size[size].add(name)
    return by_size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('game')
    ap.add_argument('--mods')
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--from', dest='t0', type=float)
    ap.add_argument('--to', dest='t1', type=float)
    a = ap.parse_args()
    by_size = index([a.game] + ([a.mods] if a.mods else []))

    def name(kind, size):
        if kind == 'stream':
            return '(stream)'
        names = sorted(by_size.get(size, ()))
        if not names:
            return f'? ({size} bytes)'
        return ' | '.join(names[:3]) + (' ...' if len(names) > 3 else '')

    rx = re.compile(r'\[\s*([\d.]+)\] (play|stream) \S+?(?: \((\d+) bytes\))?\r?$')
    starts = []
    for line in open(a.log, encoding='utf-8', errors='replace'):
        m = rx.match(line)
        if m:
            kind = m.group(2)
            starts.append((float(m.group(1)), kind, name(kind, int(m.group(3) or 0))))

    print('most started:')
    for n, count in collections.Counter(s[2] for s in starts).most_common(a.top):
        print(f'  {count:5d}  {n}')
    print('\nstarts per 5 s window (the most started in it):')
    windows = collections.defaultdict(collections.Counter)
    for t, _, n in starts:
        windows[int(t // 5) * 5][n] += 1
    for w in sorted(windows):
        top = ', '.join(f'{n} x{c}' for n, c in windows[w].most_common(3))
        print(f'  {w:6d}-{w + 5:<6d} {sum(windows[w].values()):4d}  {top}')
    if a.t0 is not None or a.t1 is not None:
        t0 = a.t0 if a.t0 is not None else 0
        t1 = a.t1 if a.t1 is not None else float('inf')
        print(f'\nstarts from {t0:g} to {t1:g} s:')
        for t, kind, n in starts:
            if t0 <= t <= t1:
                print(f'  {t:9.2f} {kind:6s} {n}')


main()
