"""Profile the shape of the checker's live interpretations as it reads a program.

The checker never commits to one reading of the code. After `f(` it cannot know
whether the argument will be a number, a string or another call, so it carries every
still-possible reading forward at once. Cost is roughly (number of readings) x
(characters), so the question this script answers is:

    how many readings are alive, how many of them are actually different,
    and how does that change as the program grows?

    python3 profile_states.py corpus/programs/.../foo.ts
    python3 profile_states.py --states 1001          # pick a slow one from results
    python3 profile_states.py foo.ts --stride 5      # sample every 5th character

Writes a per-position CSV (--out) and prints a summary.

A "reading" here is a whole root-to-leaf path, not just a leaf. Two leaves that look
alike but sit under different parents are not interchangeable: what may follow them
differs. So every signature covers the entire path.

Needs Python 3.11 and: frozenlist regex termcolor frozendict
"""

import argparse
import csv
import os
import sys
import time
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.join(HERE, "..", "type-constrained-code-generation")
sys.path.insert(0, REPO)
CORPUS_DIR = os.path.join(HERE, "corpus")
END_MARKER = "```"

from dataclasses import fields, is_dataclass  # noqa: E402

from typesafe_llm.parser.parser_base import (  # noqa: E402
    IncrementalParsingState,
    incremental_parse,
)
from typesafe_llm.parser.parser_ts import custom_end_initial_state  # noqa: E402

# Fields recording what has already been parsed, rather than what may still come.
# The "future" signature drops them.
HISTORY_FIELDS = {"parsed_states", "accepted", "parsed_code"}

# The frontier of open alternatives; handled by the path walk, never inside a node key.
CHILD_FIELD = "active"


def children(state):
    """Sub-readings held by this state.

    `active` is the open frontier. Some states (ExpressionParserState) keep a *list*
    there, which is real branching the top-level count does not show.
    """
    active = getattr(state, CHILD_FIELD, None)
    if isinstance(active, (list, tuple)):
        return [c for c in active if isinstance(c, IncrementalParsingState)]
    if isinstance(active, IncrementalParsingState):
        return [active]
    return []


def readings(state, prefix=()):
    """Every root-to-leaf path. One path = one reading of the code."""
    path = prefix + (state,)
    kids = children(state)
    if not kids:
        yield path
        return
    for kid in kids:
        yield from readings(kid, path)


def _value_key(value, keep_history):
    if isinstance(value, IncrementalParsingState):
        return node_key(value, keep_history)
    if isinstance(value, (list, tuple)):
        return tuple(_value_key(v, keep_history) for v in value)
    if isinstance(value, dict):
        # Scope: names and their types matter, insertion order does not.
        return tuple(sorted((str(k), str(v)) for k, v in value.items()))
    return repr(value)


def node_key(state, keep_history):
    """Describe one state's own fields, excluding its children."""
    if not is_dataclass(state):
        return repr(state)
    parts = [type(state).__name__]
    for field in fields(state):
        if field.name == CHILD_FIELD:
            continue
        if not keep_history and field.name in HISTORY_FIELDS:
            continue
        try:
            value = getattr(state, field.name)
        except AttributeError:
            continue
        parts.append((field.name, _value_key(value, keep_history)))
    return tuple(parts)


def signature(path, keep_history):
    """A hashable description of one reading.

    keep_history=True  (L1): full structure, including what has already been parsed.
                             Two readings match only if structurally identical, so
                             merging them is obviously safe and needs no argument.
    keep_history=False (L2): drops the parsed history, keeping what bears on future
                             input -- grammar position, expected type, scope. This is
                             what a merging parser could collapse, but it is a claim
                             that needs checking, not a fact.
    """
    return tuple(node_key(state, keep_history) for state in path)


# Almost every state has a `typ` field defaulting to AnyPType, which prints as
# "unknown". Only ExpressionParserState sets a concrete expected type, so keeping the
# innermost non-None value returns "unknown" nearly always: a leaf's default clobbers
# the meaningful type set further out. Keep the innermost *concrete* type instead.
UNKNOWN_TYPE = "unknown"


def expected_type(path):
    """The innermost concrete type this reading is waiting for, or None."""
    found = None
    for state in path:
        typ = getattr(state, "typ", None)
        if typ is None:
            continue
        text = str(typ)
        if text == UNKNOWN_TYPE:
            continue
        found = text
    return found


def measure(parser_state, max_signatures):
    """Shape metrics for one position."""
    paths = [p for top in parser_state.active_states for p in readings(top)]
    result = {
        "top_level": len(parser_state.active_states),
        "readings": len(paths),
        "distinct_l1": "",
        "distinct_l2": "",
        "distinct_types": "",
    }
    # Signatures walk every path in full, so they get expensive exactly where the
    # counts are most interesting. Above the cap, record counts only.
    if len(paths) <= max_signatures:
        result["distinct_l1"] = len({signature(p, True) for p in paths})
        result["distinct_l2"] = len({signature(p, False) for p in paths})
        result["distinct_types"] = len({expected_type(p) for p in paths})
    return result


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
    print(f"picked {best['path']} ({best['states']} top-level states, {best['seconds']}s)")
    return os.path.join(CORPUS_DIR, best["path"])


def summarize(rows, stride, out_path):
    counts = [r["readings"] for r in rows]
    peak = max(rows, key=lambda r: r["readings"])
    work = sum(counts) * stride

    def per_position(bottom):
        """Duplication measured at each position separately.

        Summing readings and distinct counts across the whole program and dividing
        once would hide local duplication: a position holding 24 readings that
        collapse to 8 is invisible next to hundreds of positions holding 2 readings
        that do not collapse. Cost concentrates at the crowded positions, so those
        are the ones that matter.
        """
        vals = [(r["readings"] / r[bottom], r) for r in rows
                if r[bottom] not in ("", 0)]
        if not vals:
            return None
        vals.sort(key=lambda v: v[0])
        at_peak = next((v for v, r in vals if r["index"] == peak["index"]), float("nan"))
        return {
            "median": vals[len(vals) // 2][0],
            "max": vals[-1][0],
            "at_peak": at_peak,
            "n_dup": sum(1 for v, _ in vals if v > 1.0),
            "n": len(vals),
        }

    print(f"  work (readings summed over characters): {work:,}")
    print(f"  readings: median {sorted(counts)[len(counts)//2]}, "
          f"peak {peak['readings']} at char {peak['index']} ({peak['char']})")
    nesting = per_position("top_level")
    if nesting:
        print(f"  top-level count understates readings by: "
              f"median {nesting['median']:.1f}x, max {nesting['max']:.1f}x")

    for label, column in (("L1 (identical readings)", "distinct_l1"),
                          ("L2 (same future)      ", "distinct_l2")):
        stats = per_position(column)
        if not stats:
            continue
        print(f"\n  {label} duplication, per position:")
        print(f"    median {stats['median']:.2f}x, max {stats['max']:.2f}x, "
              f"at peak {stats['at_peak']:.2f}x")
        print(f"    positions with any duplication: {stats['n_dup']}/{stats['n']}")

    types = [r["distinct_types"] for r in rows if r["distinct_types"] != ""]
    if types:
        print(f"  distinct expected types: max {max(types)}, "
              f"median {sorted(types)[len(types)//2]}")
    print("\n  biggest jumps in readings:")
    jumps = [(b["readings"] / a["readings"], b)
             for a, b in zip(rows, rows[1:]) if a["readings"]]
    for factor, row in sorted(jumps, key=lambda j: -j[0])[:5]:
        print(f"    x{factor:>6.1f} at char {row['index']:>5} {row['char']:<6} "
              f"-> {row['readings']} readings")
    print(f"\n  per-position detail: {out_path}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("program", nargs="?")
    ap.add_argument("--states", type=int, default=0,
                    help="pick the slowest program with at least this many states")
    ap.add_argument("--results", default="results_quick.csv")
    ap.add_argument("--stride", type=int, default=1,
                    help="measure every Nth character (1 = every character)")
    ap.add_argument("--max-signatures", type=int, default=3000,
                    help="skip signature work above this many readings")
    ap.add_argument("--out", default=os.path.join(HERE, "results_states.csv"))
    args = ap.parse_args()

    path = pick_program(args)
    text = open(path).read().rstrip("\n")
    print(f"{os.path.basename(path)}: {len(text)} characters\n")

    parser_state = custom_end_initial_state(END_MARKER, {})
    rows = []
    started = time.perf_counter()

    for i, char in enumerate(text):
        parser_state = incremental_parse(parser_state, char)
        if not parser_state.active_states:
            print(f"checker rejected at character {i}: {char!r}\n")
            break
        if i % args.stride and i != len(text) - 1:
            continue

        # Note: there is deliberately no "frontier survival" metric here. Comparing
        # signatures between consecutive positions always gives ~0 overlap, because
        # signatures include fields that accumulate the consumed text (id_name grows
        # '' -> 'n' -> 'nu' as an identifier is read). Measuring how many readings
        # die immediately needs lineage tracking, which this functional design does
        # not expose.
        rows.append({"index": i, "char": repr(char),
                     **measure(parser_state, args.max_signatures)})

    if not rows:
        raise SystemExit("no positions measured")

    with open(args.out, "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)

    print(f"measured {len(rows)} positions in {time.perf_counter()-started:.1f}s\n")
    summarize(rows, args.stride, os.path.relpath(args.out, HERE))


if __name__ == "__main__":
    main()
