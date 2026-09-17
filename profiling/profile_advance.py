"""Time the PLDI type checker on programs from the corpus.

Run extract_corpus.py first to build corpus/. This reads corpus/index.csv, selects
programs, feeds each to the checker, and records how long it took.

    # everything (30k programs, use several cores)
    ./venv/bin/python profile_advance.py --jobs 8

    # only gemma-2-2b, type-constrained
    ./venv/bin/python profile_advance.py --include programs/google_gemma-2-2b-it/c/

    # only mbpp synthesis from that model, biggest programs first
    ./venv/bin/python profile_advance.py \
        --include programs/google_gemma-2-2b-it/c/mbpp_synth --min-chars 500

Results append to a CSV, so a run can be interrupted and resumed: programs already
recorded are skipped.

Needs Python 3.11 and: frozenlist regex termcolor frozendict
"""

import argparse
import csv
import os
import random
import signal
import statistics
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.join(HERE, "..", "type-constrained-code-generation")
sys.path.insert(0, REPO)

CORPUS_DIR = os.path.join(HERE, "corpus")
INDEX_FILE = os.path.join(CORPUS_DIR, "index.csv")

# The checker was run with this end-of-code marker during generation.
END_MARKER = "```"

FIELDS = ["path", "chars", "chars_read", "seconds", "ms_per_char", "states", "status"]


class Timeout(Exception):
    pass


def _alarm(signum, frame):
    raise Timeout()


def check_one(path, timeout):
    """Run the checker over one program file.

    Returns a result dict. status is one of:
      ok            - checker read the whole program
      stopped_early - checker rejected something partway through
      timeout       - gave up after the time limit
    """
    from typesafe_llm.parser.parser_ts import (
        custom_end_initial_state,
        incremental_ts_parse,
    )

    text = open(os.path.join(CORPUS_DIR, path)).read()
    state = custom_end_initial_state(END_MARKER, {})

    signal.signal(signal.SIGALRM, _alarm)
    signal.setitimer(signal.ITIMER_REAL, timeout)
    start = time.perf_counter()
    try:
        result = incremental_ts_parse(state, text)
        seconds = time.perf_counter() - start
        chars_read, states = len(result.parsed_code), len(result)
        status = "ok" if chars_read >= len(text.rstrip()) else "stopped_early"
    except Timeout:
        seconds = time.perf_counter() - start
        chars_read, states, status = 0, 0, "timeout"
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)

    return {
        "path": path,
        "chars": len(text),
        "chars_read": chars_read,
        "seconds": round(seconds, 4),
        "ms_per_char": round(seconds * 1000.0 / chars_read, 4) if chars_read else "",
        "states": states,
        "status": status,
    }


def _worker(args):
    return check_one(*args)


def load_index(args):
    """Programs from index.csv matching the filters, as (path, chars) pairs."""
    rows = []
    with open(INDEX_FILE) as handle:
        for row in csv.DictReader(handle):
            path, chars = row["path"], int(row["chars"])
            if args.include and not any(path.startswith(p) for p in args.include):
                continue
            if args.exclude and any(path.startswith(p) for p in args.exclude):
                continue
            if args.constrained is not None:
                if int(row["constrained"]) != args.constrained:
                    continue
            if args.model and not any(m in row["model"] for m in args.model):
                continue
            if args.task and row["task"] not in args.task:
                continue
            if args.dataset and row["dataset"] not in args.dataset:
                continue
            if chars < args.min_chars:
                continue
            if args.max_chars and chars > args.max_chars:
                continue
            rows.append((path, chars))

    if args.shuffle:
        random.Random(args.seed).shuffle(rows)
    elif args.longest_first:
        rows.sort(key=lambda r: -r[1])
    else:
        rows.sort(key=lambda r: r[0])

    if args.limit:
        rows = rows[: args.limit]
    return rows


def already_done(out_file):
    if not os.path.exists(out_file):
        return set()
    with open(out_file) as handle:
        return {row["path"] for row in csv.DictReader(handle)}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--include", action="append", default=[],
                    help="only paths starting with this (repeatable), "
                         "e.g. programs/google_gemma-2-2b-it/c/")
    ap.add_argument("--exclude", action="append", default=[],
                    help="skip paths starting with this (repeatable)")
    ap.add_argument("--constrained", dest="constrained", action="store_const", const=1,
                    default=1,
                    help="only programs generated with the checker on (the default: "
                         "these are the only ones the checker ever ran on)")
    ap.add_argument("--unconstrained", dest="constrained", action="store_const", const=0,
                    help="only programs generated without the checker")
    ap.add_argument("--all-programs", dest="constrained", action="store_const", const=None,
                    help="both constrained and unconstrained")
    ap.add_argument("--model", action="append", default=[],
                    help="substring match on model name (repeatable), e.g. gemma-2-2b")
    ap.add_argument("--task", action="append", default=[],
                    help="synth | translate | repair-all (repeatable)")
    ap.add_argument("--dataset", action="append", default=[],
                    help="mbpp | humaneval (repeatable)")
    ap.add_argument("--min-chars", type=int, default=0)
    ap.add_argument("--max-chars", type=int, default=0, help="0 = no limit")
    ap.add_argument("--limit", type=int, default=0, help="0 = all matching programs")
    ap.add_argument("--timeout", type=float, default=60.0,
                    help="seconds per program before giving up (default 60)")
    ap.add_argument("--jobs", type=int, default=1, help="parallel worker processes")
    ap.add_argument("--out", default=os.path.join(HERE, "results_advance.csv"))
    ap.add_argument("--shuffle", action="store_true", help="random order")
    ap.add_argument("--longest-first", action="store_true")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--fresh", action="store_true", help="ignore previous results")
    args = ap.parse_args()

    if not os.path.exists(INDEX_FILE):
        raise SystemExit(f"no corpus index at {INDEX_FILE}; run extract_corpus.py first")

    selected = load_index(args)
    done = set() if args.fresh else already_done(args.out)
    todo = [(p, c) for p, c in selected if p not in done]

    print(f"{len(selected)} programs match; {len(done & {p for p, _ in selected})} already done; "
          f"{len(todo)} to run")
    print(f"timeout {args.timeout}s per program, {args.jobs} job(s) -> "
          f"{os.path.relpath(args.out, HERE)}\n")
    if not todo:
        return

    write_header = args.fresh or not os.path.exists(args.out)
    mode = "w" if args.fresh else "a"
    results = []
    started = time.time()

    with open(args.out, mode, newline="") as out_handle:
        writer = csv.DictWriter(out_handle, fieldnames=FIELDS)
        if write_header:
            writer.writeheader()

        tasks = [(path, args.timeout) for path, _ in todo]
        if args.jobs > 1:
            import multiprocessing

            with multiprocessing.Pool(args.jobs) as pool:
                stream = pool.imap_unordered(_worker, tasks, chunksize=4)
                for i, res in enumerate(stream, 1):
                    writer.writerow(res)
                    out_handle.flush()
                    results.append(res)
                    report(i, len(tasks), res, started)
        else:
            for i, task in enumerate(tasks, 1):
                res = _worker(task)
                writer.writerow(res)
                out_handle.flush()
                results.append(res)
                report(i, len(tasks), res, started)

    summarize(results)


def report(i, total, res, started):
    """Print slow or rejected programs as they happen, plus periodic progress."""
    notable = res["status"] != "ok" or res["seconds"] > 5
    if notable or i % 200 == 0 or i == total:
        rate = i / max(time.time() - started, 1e-9)
        eta = (total - i) / rate if rate else 0
        print(
            f"[{i}/{total}] {res['seconds']:>7.2f}s  {res['status']:<13} "
            f"states={res['states']:<5} {res['path'][-60:]}"
            + (f"   ({eta/60:.0f} min left)" if not notable else ""),
            flush=True,
        )


def summarize(results):
    ok = [r for r in results if r["status"] == "ok"]
    per_char = [r["ms_per_char"] for r in ok if r["ms_per_char"] != ""]
    total_time = sum(r["seconds"] for r in results)
    print(f"\nran {len(results)} programs in {total_time/60:.1f} min")
    for status in ("ok", "stopped_early", "timeout"):
        n = sum(1 for r in results if r["status"] == status)
        if n:
            print(f"  {status:<14} {n}")
    if not per_char:
        return
    per_char.sort()
    print(f"\nms/char over {len(per_char)} fully-read programs:")
    for label, idx in (("median", len(per_char) // 2),
                       ("90%", int(0.90 * len(per_char))),
                       ("99%", int(0.99 * len(per_char))),
                       ("max", len(per_char) - 1)):
        print(f"  {label:>6}: {per_char[idx]:.2f}")
    print(f"  mean  : {statistics.mean(per_char):.2f}")
    slowest = max(ok, key=lambda r: r["seconds"])
    print(f"\nslowest: {slowest['seconds']:.1f}s  states={slowest['states']}  {slowest['path']}")


if __name__ == "__main__":
    main()
