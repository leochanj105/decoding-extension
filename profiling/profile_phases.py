"""Break the checker's time down by phase.

Runs the checker under cProfile and groups every function into one phase:

  parser advance     reading a character, updating the live interpretations
  type derivation    working out what type an expression has so far
  reachability       "can I still reach the type I need from here?"
  scope lookup       scanning the variables/functions in scope
  object copying     rebuilding the frozen state objects on every transition
  other              everything else (printed individually so it can be classified)

Uses each function's *own* time, not time including what it called, so the phases
do not double-count: the parser calls type derivation, which calls reachability.

    python3 profile_phases.py corpus/programs/.../foo.ts
    python3 profile_phases.py --states 1001        # pick a program from results CSV

Needs Python 3.11 and: frozenlist regex termcolor frozendict
"""

import argparse
import csv
import cProfile
import os
import pstats
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.join(HERE, "..", "type-constrained-code-generation")
sys.path.insert(0, REPO)
CORPUS_DIR = os.path.join(HERE, "corpus")
END_MARKER = "```"

# (filename, function-name substring) -> phase. First match wins.
RULES = [
    ("types_ts.py", "_reachable_bfs", "reachability"),
    ("types_ts.py", "_reachable", "reachability"),
    ("types_ts.py", "reachable", "reachability"),
    ("types_ts.py", "any_reachable", "reachability"),
    ("types_ts.py", "__ge__", "type derivation"),
    ("types_ts.py", "instantiate_type_params", "type derivation"),
    ("types_ts.py", "extract_type_params", "type derivation"),
    ("types_ts.py", "nesting_depth", "type derivation"),
    ("types_ts.py", "root_values", "type derivation"),
    ("types_base.py", "", "type derivation"),
    (None, "derivable", "type derivation"),
    (None, "described_type", "type derivation"),
    (None, "parse_char", "parser advance"),
    (None, "init_class_at_pos_hook", "parser advance"),
    (None, "init_classes", "parser advance"),
    (None, "incremental_parse", "parser advance"),
    (None, "incremental_ts_parse", "parser advance"),
    (None, "sum_list", "parser advance"),
    ("parser_ts.py", "__post_init__", "scope lookup"),
    ("parser_base.py", "__post_init__", "scope lookup"),
    ("util.py", "union_dict", "scope lookup"),
    ("util.py", "update_keys", "scope lookup"),
    ("util.py", "intersection_dict", "scope lookup"),
    ("dataclasses.py", "", "object copying"),
    ("copy.py", "", "object copying"),
    (None, "__hash__", "object copying"),
    (None, "__eq__", "object copying"),
]

PHASE_ORDER = ["parser advance", "type derivation", "reachability", "scope lookup",
               "object copying", "other"]

# Comprehensions and generators report as <genexpr>/<listcomp>, so fall back to
# whichever phase their file belongs to.
FILE_PHASE = {
    "parser_base.py": "parser advance",
    "parser_ts.py": "parser advance",
    "parser_ts_types.py": "parser advance",
    "parser_shared.py": "parser advance",
    "types_ts.py": "type derivation",
    "types_base.py": "type derivation",
    "util.py": "scope lookup",
    "trie.py": "other",
}


def classify(filename, funcname):
    base = os.path.basename(filename)
    for want_file, want_func, phase in RULES:
        if want_file and want_file != base:
            continue
        if want_func and want_func not in funcname:
            continue
        if not want_file and not want_func:
            continue
        return phase
    if funcname.startswith("<") and funcname.endswith(">"):
        return FILE_PHASE.get(base, "other")
    return FILE_PHASE.get(base, "other")


def pick_program(args):
    if args.program:
        return args.program
    results = os.path.join(HERE, args.results)
    if not os.path.exists(results):
        raise SystemExit(f"no {args.results}; pass a .ts path or run profile_advance.py")
    best = None
    for row in csv.DictReader(open(results)):
        if row["status"] != "ok" or not row["states"]:
            continue
        states = int(row["states"])
        if states < args.states:
            continue
        # among programs above the state threshold, take the slowest
        if best is None or float(row["seconds"]) > float(best["seconds"]):
            best = row
    if not best:
        raise SystemExit(f"no program with >= {args.states} states in {args.results}")
    print(f"picked {best['path']}  (states={best['states']}, {best['seconds']}s)")
    return os.path.join(CORPUS_DIR, best["path"])


def run(path):
    from typesafe_llm.parser.parser_ts import (
        custom_end_initial_state,
        incremental_ts_parse,
    )

    text = open(path).read().rstrip("\n")
    state = custom_end_initial_state(END_MARKER, {})

    # Time it once without the profiler, so we can report the profiler's overhead.
    t0 = time.perf_counter()
    result = incremental_ts_parse(custom_end_initial_state(END_MARKER, {}), text)
    plain = time.perf_counter() - t0

    profiler = cProfile.Profile()
    profiler.enable()
    incremental_ts_parse(state, text)
    profiler.disable()
    return text, result, plain, pstats.Stats(profiler)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("program", nargs="?", help="path to a .ts file")
    ap.add_argument("--states", type=int, default=0,
                    help="pick the slowest program with at least this many live states")
    ap.add_argument("--results", default="results_quick.csv")
    ap.add_argument("--top", type=int, default=12, help="how many functions to list")
    args = ap.parse_args()

    path = pick_program(args)
    text, result, plain, stats = run(path)

    print(f"\n{os.path.basename(path)}")
    print(f"  {len(text)} chars, {len(result)} live states at end")
    print(f"  {plain:.2f}s without profiler, {plain*1000/max(len(text),1):.2f} ms/char")

    entries = list(stats.stats.items())

    # Classify everything defined in Python first.
    phase_of = {}
    for key in (k for k, _ in entries):
        if key[0] != "~":
            phase_of[key] = classify(key[0], key[2])

    # Built-ins like getattr and hash are called from every phase, so leaving them
    # unattributed hides a third of the time. Charge each to the phase that calls it
    # most often.
    for key, value in entries:
        if key[0] != "~":
            continue
        tally = {}
        for caller_key, caller_stats in (value[4] or {}).items():
            caller_phase = phase_of.get(caller_key) or classify(caller_key[0], caller_key[2])
            tally[caller_phase] = tally.get(caller_phase, 0) + caller_stats[0]
        phase_of[key] = max(tally, key=tally.get) if tally else "other"

    by_phase = {}
    by_func = []
    total_self = 0.0
    for key, (calls, _nc, self_time, _cum, _cb) in entries:
        filename, _lineno, funcname = key
        phase = phase_of.get(key, "other")
        acc = by_phase.setdefault(phase, [0.0, 0])
        acc[0] += self_time
        acc[1] += calls
        total_self += self_time
        by_func.append((self_time, calls, phase, os.path.basename(filename), funcname))

    profiled = sum(v[0] for v in by_phase.values())
    print(f"  {profiled:.2f}s under profiler ({profiled/max(plain,1e-9):.1f}x overhead)\n")

    print(f"{'phase':<18}{'self time':>11}{'share':>9}{'calls':>14}")
    for phase in PHASE_ORDER:
        if phase not in by_phase:
            continue
        seconds, calls = by_phase[phase]
        print(f"{phase:<18}{seconds:>10.2f}s{100*seconds/total_self:>8.1f}%{calls:>14,}")
    print(f"{'TOTAL':<18}{total_self:>10.2f}s{100:>8.1f}%")

    print(f"\ntop {args.top} functions by own time:")
    by_func.sort(reverse=True)
    for self_time, calls, phase, filename, funcname in by_func[: args.top]:
        print(f"  {self_time:>7.2f}s {100*self_time/total_self:>5.1f}%  {phase:<16} "
              f"{filename}:{funcname[:40]}  ({calls:,} calls)")

    other = [f for f in by_func if f[2] == "other"][:5]
    if other:
        print("\nlargest unclassified ('other') functions:")
        for self_time, calls, _phase, filename, funcname in other:
            print(f"  {self_time:>7.2f}s  {filename}:{funcname[:45]}  ({calls:,} calls)")


if __name__ == "__main__":
    main()
