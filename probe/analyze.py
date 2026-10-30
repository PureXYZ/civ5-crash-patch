"""Summarise civprobe output.

Usage: python analyze.py <civprobe folder> [session]
With no session, the most recent one in the folder is used.
"""
import csv
import glob
import os
import sys
from collections import defaultdict

LOW = 0x80000000
MB = 1024 * 1024


def latest_session(folder):
    # Every session has an events log (samples only with Telemetry=1). Newest by file time: the
    # local-time stamps in the names don't sort right when the clocks go back.
    logs = glob.glob(os.path.join(folder, "events_*.log"))
    if not logs:
        sys.exit("no events_*.log in " + folder)
    return os.path.basename(max(logs, key=os.path.getmtime))[len("events_"):-len(".log")]


def read_rows(path):
    """The CSV's rows, without a last row cut short (the game ended while it was written)."""
    with open(path, newline="") as fh:
        return [r for r in csv.DictReader(fh) if None not in r.values()]


def summarise_samples(path):
    if not os.path.exists(path):
        print("no samples file (Telemetry=0)")
        return
    rows = [r for r in read_rows(path) if all(v != "" for v in r.values())]
    if not rows:
        print("no samples")
        return
    f = lambda r, k: float(r[k])
    last = rows[-1]
    print(f"samples: {len(rows)} over {f(last, 't_s') / 60:.1f} min (last tag: {last['tag']})")
    keys = [
        ("low_commit_mb", max), ("low_reserve_mb", max), ("low_free_mb", min), ("low_largest_free_mb", min),
        ("high_commit_mb", max), ("high_free_mb", min), ("high_largest_free_mb", min),
        ("image_commit_mb", max), ("mapped_commit_mb", max), ("private_commit_mb", max),
        ("lua_bytes_mb", max), ("lua_peak_mb", max), ("lua_heap_commit_mb", max), ("lua_heap_reserve_mb", max),
        ("crt_heap_alloc_mb", max), ("crt_heap_commit_mb", max), ("crt_heap_reserve_mb", max),
        ("process_heap_commit_mb", max), ("other_heaps_commit_mb", max), ("heaps", max),
    ]
    print(f"  {'metric':28s} {'first':>9s} {'extreme':>9s} {'last':>9s}")
    for k, agg in keys:
        vals = [f(r, k) for r in rows]
        print(f"  {k:28s} {vals[0]:9.1f} {agg(vals):9.1f} {vals[-1]:9.1f}   ({'max' if agg is max else 'min'})")

    print("\ntimeline (every ~5 min):")
    cols = ["low_commit_mb", "low_reserve_mb", "low_largest_free_mb", "high_commit_mb",
            "lua_bytes_mb", "lua_heap_commit_mb", "crt_heap_commit_mb", "other_heaps_commit_mb"]
    print("  " + " ".join(f"{c[:-3]:>12s}" for c in ["t_min___"] + cols))
    next_t = 0
    for r in rows:
        t = f(r, "t_s")
        if t >= next_t or r is last:
            print(f"  {t / 60:12.1f} " + " ".join(f"{f(r, c):12.1f}" for c in cols))
            next_t = t + 300


def owner_key(r):
    owner = r["owner"]
    if r["type"] == "image":
        return "image: " + (owner or "?")
    if r["type"] == "mapped":
        return "mapped: " + (owner or "?")
    if owner.startswith("heap:"):
        return owner
    return "private (unattributed)"


def summarise_vamap(path):
    rows = read_rows(path)
    used = defaultdict(lambda: [0, 0, 0, 0])  # low commit, low reserve, high commit, high reserve
    alloc_sizes = defaultdict(int)             # private allocations by allocation base
    free_low = []
    for r in rows:
        base = int(r["base"], 16)
        size = int(r["size_kb"]) * 1024
        if r["state"] == "free":
            if base < LOW:
                free_low.append(min(size, LOW - base))
            continue
        k = owner_key(r)
        slot = (0 if base < LOW else 2) + (0 if r["state"] == "commit" else 1)
        used[k][slot] += size
        if k == "private (unattributed)":
            alloc_sizes[(int(r["alloc_base"], 16), r["state"])] += size

    print(f"\n== {os.path.basename(path)}")
    print(f"  {'owner':44s} {'low commit':>10s} {'low resv':>10s} {'high commit':>11s} {'high resv':>10s}  (MB)")
    for k, v in sorted(used.items(), key=lambda kv: -(kv[1][0] + kv[1][1]))[:25]:
        print(f"  {k[:44]:44s} {v[0] / MB:10.1f} {v[1] / MB:10.1f} {v[2] / MB:11.1f} {v[3] / MB:10.1f}")

    # Unattributed private memory: how much is in big blocks vs. many small ones.
    by_base = defaultdict(int)
    for (ab, _), s in alloc_sizes.items():
        by_base[ab] += s
    buckets = [(64 << 10, "<64K"), (1 << 20, "64K-1M"), (16 << 20, "1M-16M"), (1 << 40, ">=16M")]
    hist = defaultdict(lambda: [0, 0])
    for ab, s in by_base.items():
        for limit, name in buckets:
            if s < limit:
                hist[name][0] += 1
                hist[name][1] += s
                break
    print("  unattributed private allocations by size:")
    for _, name in buckets:
        n, s = hist[name]
        print(f"    {name:8s} count={n:6d}  total={s / MB:8.1f} MB")
    big = sorted(by_base.items(), key=lambda kv: -kv[1])[:8]
    print("  largest unattributed allocations: " + ", ".join(f"0x{ab:08x}={s / MB:.0f}MB" for ab, s in big))

    free_low.sort(reverse=True)
    print(f"  low 2 GB free: {sum(free_low) / MB:.1f} MB in {len(free_low)} blocks; "
          f"largest: {', '.join(f'{s / MB:.1f}' for s in free_low[:5])} MB")


def module_of(frame):
    return frame.split("+")[0] if frame else "(none)"


def summarise_allocs(folder, session):
    files = glob.glob(os.path.join(folder, f"allocsites_{session}_*.csv"))
    if not files:
        return
    # Prefer the final report; otherwise the most recent one.
    exit_file = [f for f in files if f.endswith("_exit.csv")]
    path = exit_file[0] if exit_file else max(files, key=os.path.getmtime)
    rows = read_rows(path)
    total = sum(float(r["est_live_mb"]) for r in rows)
    print(f"\n== live CRT allocations by caller: {os.path.basename(path)} (est total {total:.1f} MB)")
    for pos in ("frame0", "frame1", "frame2"):
        by = defaultdict(float)
        for r in rows:
            by[module_of(r[pos])] += float(r["est_live_mb"])
        top = sorted(by.items(), key=lambda kv: -kv[1])[:8]
        print(f"  by {pos} module: " + ", ".join(f"{m} {v:.1f}" for m, v in top))
    print(f"  {'est MB':>8s} {'big MB':>8s} {'blocks':>9s} kind     stack")
    for r in rows[:30]:
        frames = " < ".join(r[f"frame{i}"] for i in range(5) if r[f"frame{i}"])
        print(f"  {float(r['est_live_mb']):8.1f} {float(r['big_mb']):8.1f} {int(r['est_blocks']):9d} "
              f"{r['kind']:8s} {frames}")

    calls = path.replace("allocsites_", "alloccalls_")
    if os.path.exists(calls):
        crow = read_rows(calls)
        print(f"\n  top call sites by cumulative bytes allocated (churn):")
        for r in crow[:12]:
            print(f"  {float(r['total_mb']):10.1f} MB {int(r['calls']):10d} calls  {r['kind']:8s} {r['site']}")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    folder = sys.argv[1]
    session = sys.argv[2] if len(sys.argv) > 2 else latest_session(folder)
    print(f"session {session}\n")
    events = os.path.join(folder, f"events_{session}.log")
    if os.path.exists(events):
        print(open(events, encoding="utf-8", errors="replace").read())
    summarise_samples(os.path.join(folder, f"samples_{session}.csv"))
    for path in sorted(glob.glob(os.path.join(folder, f"vamap_{session}_*.csv")), key=os.path.getmtime):
        summarise_vamap(path)
    summarise_allocs(folder, session)


if __name__ == "__main__":
    main()
