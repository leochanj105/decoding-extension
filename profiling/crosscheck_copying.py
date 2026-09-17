"""Cross-check how much of the checker's time is spent copying state objects.

profile_phases.py puts object copying around 45%, but it uses cProfile, which
charges per function call and so overstates code that makes many tiny calls --
exactly what copying is. This measures the same thing without instrumenting
everything: it wraps only `dataclasses.replace` in a timer and leaves the rest of
the checker untouched.

The parser states are frozen dataclasses, so every character transition rebuilds
them through `replace` rather than mutating in place.

    python3 crosscheck_copying.py corpus/programs/.../foo.ts

Reports the measured time inside `replace`, as a share of an uninstrumented run.

Needs Python 3.11 and: frozenlist regex termcolor frozendict
"""

import argparse
import csv
import dataclasses
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.join(HERE, "..", "type-constrained-code-generation")
sys.path.insert(0, REPO)
CORPUS_DIR = os.path.join(HERE, "corpus")
END_MARKER = "```"

ORIGINAL_REPLACE = dataclasses.replace
TALLY = {"calls": 0, "seconds": 0.0}


def timed_replace(*args, **kwargs):
    start = time.perf_counter()
    result = ORIGINAL_REPLACE(*args, **kwargs)
    TALLY["seconds"] += time.perf_counter() - start
    TALLY["calls"] += 1
    return result


def patch():
    """Swap in the timer everywhere the checker imported `replace`.

    The parser modules do `from dataclasses import replace`, so they hold their own
    reference and patching dataclasses.replace alone would miss them.
    """
    patched = []
    for name, module in list(sys.modules.items()):
        if not name.startswith("typesafe_llm"):
            continue
        if getattr(module, "replace", None) is ORIGINAL_REPLACE:
            module.replace = timed_replace
            patched.append(name)
    dataclasses.replace = timed_replace
    return patched


def unpatch(patched):
    for name in patched:
        sys.modules[name].replace = ORIGINAL_REPLACE
    dataclasses.replace = ORIGINAL_REPLACE


def parse(text):
    from typesafe_llm.parser.parser_ts import (
        custom_end_initial_state,
        incremental_ts_parse,
    )

    return incremental_ts_parse(custom_end_initial_state(END_MARKER, {}), text)


def pick_program(args):
    if args.program:
        return args.program
    results = os.path.join(HERE, args.results)
    best = None
    for row in csv.DictReader(open(results)):
        if row["status"] != "ok" or not row["states"]:
            continue
        if int(row["states"]) < args.states:
            continue
        if best is None or float(row["seconds"]) > float(best["seconds"]):
            best = row
    if not best:
        raise SystemExit(f"no program with >= {args.states} states in {args.results}")
    print(f"picked {best['path']}  (states={best['states']})")
    return os.path.join(CORPUS_DIR, best["path"])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("program", nargs="?")
    ap.add_argument("--states", type=int, default=0)
    ap.add_argument("--results", default="results_quick.csv")
    ap.add_argument("--repeats", type=int, default=3)
    args = ap.parse_args()

    path = pick_program(args)
    text = open(path).read().rstrip("\n")

    # Warm up: first run pays import and cache costs that later runs do not.
    parse(text)

    clean = []
    for _ in range(args.repeats):
        start = time.perf_counter()
        parse(text)
        clean.append(time.perf_counter() - start)
    baseline = min(clean)

    patched = patch()
    TALLY["calls"] = TALLY["seconds"] = 0
    instrumented = []
    for _ in range(args.repeats):
        TALLY["calls"] = TALLY["seconds"] = 0
        start = time.perf_counter()
        parse(text)
        instrumented.append(time.perf_counter() - start)
    unpatch(patched)

    measured = min(instrumented)
    overhead = measured - baseline

    print(f"\n{os.path.basename(path)}  ({len(text)} chars)")
    print(f"  patched modules      : {len(patched)}")
    print(f"\n  uninstrumented run   : {baseline:.3f}s")
    print(f"  with timer on replace: {measured:.3f}s  (+{overhead:.3f}s, "
          f"{100*overhead/baseline:.1f}% overhead)")
    print(f"\n  replace() calls      : {TALLY['calls']:,}")
    print(f"  time inside replace  : {TALLY['seconds']:.3f}s")
    print(f"  per call             : {1e6*TALLY['seconds']/max(TALLY['calls'],1):.2f} us")
    print(f"\n  share of uninstrumented runtime: "
          f"{100*TALLY['seconds']/baseline:.1f}%")
    print("\n  (cProfile attributed ~45% to object copying, of which replace itself")
    print("   was ~27%. This number covers replace only, measured with one timer")
    print("   rather than instrumenting every call.)")


if __name__ == "__main__":
    main()
