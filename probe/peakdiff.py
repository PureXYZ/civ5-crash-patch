"""Shows what a transient CRT-heap spike was made of.

Compares the civprobe "peak" alloc report with a later one (default: "latest", else
"exit") from the same session. Allocations live at the peak but freed by the later
report are the spike. Usage:

    python peakdiff.py <civprobe dir> [session] [later-label]
"""
import collections
import csv
import glob
import os
import sys


def load(path):
    rows = {}
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            if None in r.values():
                continue  # a last row cut short
            key = (r["kind"],) + tuple(r["frame%d" % i] for i in range(5))
            rows[key] = rows.get(key, 0.0) + float(r["est_live_mb"])
    return rows


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    d = sys.argv[1]
    if len(sys.argv) > 2:
        session = sys.argv[2]
    else:
        peaks = sorted(glob.glob(os.path.join(d, "allocsites_*_peak.csv")), key=os.path.getmtime)
        if not peaks:
            sys.exit("no allocsites_*_peak.csv in " + d)
        session = os.path.basename(peaks[-1])[len("allocsites_"):-len("_peak.csv")]
    later_label = sys.argv[3] if len(sys.argv) > 3 else None
    peak_path = os.path.join(d, "allocsites_%s_peak.csv" % session)
    if later_label is None:
        later_label = "latest" if os.path.exists(os.path.join(d, "allocsites_%s_latest.csv" % session)) else "exit"
    later_path = os.path.join(d, "allocsites_%s_%s.csv" % (session, later_label))
    for p in (peak_path, later_path):
        if not os.path.exists(p):
            sys.exit("no %s (AllocTracking=1 writes the alloc reports)" % p)
    peak, later = load(peak_path), load(later_path)
    print("session %s: peak report %.1f MB, %s report %.1f MB" % (
        session, sum(peak.values()), later_label, sum(later.values())))

    delta = {k: v - later.get(k, 0.0) for k, v in peak.items()}
    spike = sum(v for v in delta.values() if v > 0)
    print("live at peak but gone by '%s': %.1f MB\n" % (later_label, spike))

    for title, idx in (("by allocating call (frame0)", 1), ("by its caller (frame1)", 2),
                       ("by caller's caller (frame2)", 3)):
        agg = collections.Counter()
        for k, v in delta.items():
            if v > 0:
                agg[k[idx]] += v
        print(title)
        for site, mb in agg.most_common(12):
            print("  %8.1f MB  %s" % (mb, site))
        print()

    print("top stacks (MB freed after the peak; kind; frames 0-4)")
    for k, v in sorted(delta.items(), key=lambda kv: -kv[1])[:25]:
        if v <= 0.5:
            break
        print("  %8.1f MB  %-8s %s" % (v, k[0], " < ".join(f for f in k[1:] if f)))


if __name__ == "__main__":
    main()
