"""Summarize the timing results produced by profile_advance.py.

    python3 analyze_results.py                     # reads results_quick.csv
    python3 analyze_results.py results_all.csv

Joins each timed program back to corpus/index.csv for its model, task and size, then
reports how the checker's cost is distributed and what it tracks.

The question this is built to answer: does the checker's cost follow program size, or
the number of interpretations it is holding at once (the `states` column)?
"""

import csv
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
INDEX_FILE = os.path.join(HERE, "corpus", "index.csv")

STATE_BUCKETS = [(0, 1), (1, 11), (11, 51), (51, 201), (201, 1001), (1001, 10**9)]


def label(lo, hi):
    if hi - lo == 1:
        return str(lo)
    return f"{lo}-{hi - 1}" if hi < 10**9 else f"{lo}+"


def pct(values, fraction):
    return values[min(int(fraction * len(values)), len(values) - 1)]


def load(results_file):
    index = {}
    with open(INDEX_FILE) as handle:
        for row in csv.DictReader(handle):
            index[row["path"]] = row

    rows = []
    with open(results_file) as handle:
        for row in csv.DictReader(handle):
            meta = index.get(row["path"])
            if not meta:
                continue
            rows.append(
                {
                    "path": row["path"],
                    "status": row["status"],
                    "seconds": float(row["seconds"]),
                    "chars": int(row["chars"]),
                    "chars_read": int(row["chars_read"]),
                    "states": int(row["states"]) if row["states"] else -1,
                    "ms_per_char": float(row["ms_per_char"]) if row["ms_per_char"] else None,
                    "model": meta["model"],
                    "task": meta["task"],
                    "dataset": meta["dataset"],
                }
            )
    return rows


def distribution(name, values, unit=""):
    values = sorted(values)
    if not values:
        return
    print(f"\n{name} ({len(values)} programs){unit}")
    for tag, frac in (("median", 0.50), ("75%", 0.75), ("90%", 0.90),
                      ("99%", 0.99), ("max", 1.0)):
        print(f"  {tag:>7}: {pct(values, frac):>10.2f}")
    print(f"  {'mean':>7}: {statistics.mean(values):>10.2f}")


def by_group(rows, key, value_fn, title):
    groups = {}
    for row in rows:
        groups.setdefault(row[key], []).append(row)
    print(f"\n{title}")
    print(f"  {key:<42}{'n':>6}{'median':>10}{'90%':>10}{'max':>10}")
    for name in sorted(groups):
        vals = sorted(v for v in (value_fn(r) for r in groups[name]) if v is not None)
        if not vals:
            continue
        print(
            f"  {name:<42}{len(vals):>6}{pct(vals, .5):>10.2f}"
            f"{pct(vals, .9):>10.2f}{pct(vals, 1.0):>10.2f}"
        )


def main():
    results_file = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "results_quick.csv")
    if not os.path.exists(results_file):
        raise SystemExit(f"no results at {results_file}; run profile_advance.py first")

    rows = load(results_file)
    print(f"{len(rows)} programs from {os.path.basename(results_file)}")

    print("\nstatus:")
    for status in ("ok", "rejected", "stopped_early", "timeout"):
        n = sum(1 for r in rows if r["status"] == status)
        if n:
            print(f"  {status:<14}{n:>6}  ({100.0 * n / len(rows):.1f}%)")

    done = [r for r in rows if r["status"] == "ok"]
    distribution("ms per character", [r["ms_per_char"] for r in done if r["ms_per_char"]])
    distribution("seconds per program", [r["seconds"] for r in done])
    distribution("live states at end", [float(r["states"]) for r in done])

    # The central question: what does cost track?
    print("\ncost by number of live interpretations:")
    print(f"  {'states':<12}{'n':>6}{'med ms/char':>14}{'med chars':>12}{'med seconds':>14}")
    for lo, hi in STATE_BUCKETS:
        group = [r for r in done if lo <= r["states"] < hi and r["ms_per_char"]]
        if not group:
            continue
        per_char = sorted(r["ms_per_char"] for r in group)
        chars = sorted(float(r["chars"]) for r in group)
        secs = sorted(r["seconds"] for r in group)
        print(
            f"  {label(lo, hi):<12}{len(group):>6}{pct(per_char, .5):>14.2f}"
            f"{pct(chars, .5):>12.0f}{pct(secs, .5):>14.2f}"
        )

    by_group(done, "model", lambda r: r["ms_per_char"], "ms/char by model")
    by_group(done, "task", lambda r: r["ms_per_char"], "ms/char by task")

    print("\nslowest programs:")
    for row in sorted(rows, key=lambda r: -r["seconds"])[:10]:
        per_char = f"{row['ms_per_char']:.1f}" if row["ms_per_char"] else "-"
        print(
            f"  {row['seconds']:>7.1f}s  states={row['states']:<6} chars={row['chars']:<6} "
            f"ms/char={per_char:<7} {row['status']:<13} {row['path'][-58:]}"
        )

    timeouts = [r for r in rows if r["status"] == "timeout"]
    if timeouts:
        print(f"\n{len(timeouts)} timed out; their sizes (chars):")
        sizes = sorted(r["chars"] for r in timeouts)
        print(f"  min {sizes[0]}, median {pct(sizes, .5)}, max {sizes[-1]}")


if __name__ == "__main__":
    main()
