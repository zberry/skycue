#!/usr/bin/env python3
"""Pass rule for a soak CSV (soak.sh): RSS flat and fds flat per instance.

usage: soak_eval.py CSV [--growth 0.05] [--fd-tolerance 2]
       soak_eval.py --self-test

Per instance, over its samples ordered by t_s: the window is the samples with
t_s >= 0.25 * t_max (t_max of that instance); the reference is the median RSS
of the first 3 window samples, the end the median of the last 3; the
instance passes when (end - ref) / ref < growth and every window sample's fd
count is within +-fd_tolerance of the first window sample's. Fewer than 6
window samples is a FAIL (too few samples).

Exit 0 with one PASS line per instance, 1 with FAIL <instance>: <reason>
followed by that instance's series. Python 3.6, stdlib only.
"""

import argparse
import csv
import sys

MIN_WINDOW = 6


def median(values):
    s = sorted(values)
    n = len(s)
    if n == 0:
        raise ValueError("median of nothing")
    mid = n // 2
    return s[mid] if n % 2 else (s[mid - 1] + s[mid]) / 2.0


def evaluate(samples, growth=0.05, fd_tolerance=2):
    """samples: list of (t_s, rss_kb, fds) for one instance.

    Returns (ok, reason); reason is the PASS summary or the FAIL reason."""
    samples = sorted(samples, key=lambda s: s[0])
    if not samples:
        return False, "too few samples"
    t_max = samples[-1][0]
    window = [s for s in samples if s[0] >= 0.25 * t_max]
    if len(window) < MIN_WINDOW:
        return False, "too few samples"
    ref = median([s[1] for s in window[:3]])
    end = median([s[1] for s in window[-3:]])
    if ref <= 0:
        return False, "rss reference is %g" % ref
    grew = (end - ref) / ref
    if not grew < growth:
        return False, "rss grew %.1f%%" % (grew * 100.0)
    fd0 = window[0][2]
    worst = max(window, key=lambda s: abs(s[2] - fd0))[2]
    if abs(worst - fd0) > fd_tolerance:
        return False, "fds %d -> %d" % (fd0, worst)
    fd_lo = min(s[2] for s in window)
    fd_hi = max(s[2] for s in window)
    return True, "rss %d -> %d kB (%+.1f%%), fds %d..%d, %d window samples" % (
        ref, end, grew * 100.0, fd_lo, fd_hi, len(window))


def read_csv(path):
    """Returns {instance: [row dict, ...]} with numeric fields converted."""
    series = {}
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            try:
                rec = {
                    "t_s": float(row["t_s"]),
                    "rss_kb": float(row["rss_kb"]),
                    "fds": int(row["fds"]),
                }
            except (KeyError, TypeError, ValueError):
                continue  # a partial or malformed row (killed mid-write)
            rec["raw"] = row
            series.setdefault(row["instance"], []).append(rec)
    return series


def run(path, growth, fd_tolerance):
    try:
        series = read_csv(path)
    except (IOError, OSError) as e:
        print("FAIL: cannot read %s: %s" % (path, e))
        return 1
    if not series:
        print("FAIL: no samples in %s" % path)
        return 1
    rc = 0
    for inst in sorted(series):
        rows = sorted(series[inst], key=lambda r: r["t_s"])
        ok, reason = evaluate([(r["t_s"], r["rss_kb"], r["fds"]) for r in rows],
                              growth, fd_tolerance)
        if ok:
            print("PASS %s: %s" % (inst, reason))
        else:
            rc = 1
            print("FAIL %s: %s" % (inst, reason))
            print("  t_s rss_kb fds")
            for r in rows:
                print("  %g %d %d" % (r["t_s"], r["rss_kb"], r["fds"]))
    return rc


def self_test():
    def series(rss, fds=None, step=60.0):
        fds = fds or [40] * len(rss)
        return [(i * step, float(r), f) for i, (r, f) in enumerate(zip(rss, fds))]

    n = 40
    cases = []
    # Flat.
    cases.append(("flat", series([10000] * n), True, None))
    # +20 % during the first 25 % (warm-up), flat afterwards.
    warm = [10000 + 2000 * min(1.0, i / (0.25 * (n - 1))) for i in range(n)]
    cases.append(("warm-up then flat", series(warm), True, None))
    # +6 % inside the window (flat start, flat end 6 % higher).
    rise = [10000 if i < 3 * n // 4 else 10600 for i in range(n)]
    cases.append(("6% rise", series(rise), False, "rss grew 6.0%"))
    # Just under the limit (4.9 %) still passes.
    cases.append(("4.9% rise", series([10000 if i < 3 * n // 4 else 10490 for i in range(n)]), True, None))
    # An fd step of 3 half way through the window.
    fds = [40] * n
    for i in range(n // 2, n):
        fds[i] = 43
    cases.append(("fd step", series([10000] * n, fds), False, "fds 40 -> 43"))
    # Four samples.
    cases.append(("too few", series([10000] * 4), False, "too few samples"))

    failed = 0
    for name, s, want_ok, want_reason in cases:
        try:
            ok, reason = evaluate(s, 0.05, 2)
        except Exception as e:  # the self-test reports, it does not crash
            ok, reason = None, "raised %s: %s" % (type(e).__name__, e)
        good = ok is want_ok and (want_reason is None or reason == want_reason)
        print("%s self-test %s: ok=%s reason=%s" % ("ok  " if good else "BAD ", name, ok, reason))
        if not good:
            failed += 1
            print("     expected ok=%s reason=%s" % (want_ok, want_reason))
    print("self-test: %d of %d failed" % (failed, len(cases)))
    return 1 if failed else 0


def main(argv):
    ap = argparse.ArgumentParser(description="Evaluate a soak CSV against the pass rule.")
    ap.add_argument("csv", nargs="?")
    ap.add_argument("--growth", type=float, default=0.05,
                    help="largest RSS growth over the window, as a fraction (default 0.05)")
    ap.add_argument("--fd-tolerance", type=int, default=2,
                    help="largest fd count change over the window (default 2)")
    ap.add_argument("--self-test", action="store_true", help="check the rule on synthetic series")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.csv:
        ap.error("a CSV is required (or --self-test)")
    return run(args.csv, args.growth, args.fd_tolerance)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
