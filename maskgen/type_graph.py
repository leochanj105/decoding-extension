"""Build and count the type graph: which types exist, and what reaches what.

An edge means "from a value of type A you can get to a value of type B in one
step", where a step is a member access, a call, or an operator. These are the
only ways PLDI's reachability moves between types, and they come from the eight
functions in OPERATOR_REACHABLE_TYPE_MAP -- each a pure function of the type, so
the graph does not depend on what a program declares.

The graph is infinite (string -> string[] -> string[][] -> ...), so it is closed
under a nesting-depth bound and reported per bound. Comparisons are only
meaningful at equal depth.

Why this is measured: our subset of TypeScript has to keep roughly the type and
edge structure of the real environment, or results on it will not transfer. This
turns that into a number.

    ./venv/bin/python ../maskgen/type_graph.py --max-depth 2
"""

from __future__ import annotations

import argparse
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.join(HERE, "..", "type-constrained-code-generation")
if REPO not in sys.path:
    sys.path.insert(0, REPO)

from typesafe_llm.parser import types_ts  # noqa: E402
from typesafe_llm.parser.types_ts import (  # noqa: E402
    OPERATOR_REACHABLE_TYPE_MAP,
    BooleanPType,
    NumberPType,
    StringPType,
)


def within(typ, max_depth: int) -> bool:
    try:
        arrays, functions = typ.nesting_depth
    except Exception:
        return False
    return arrays <= max_depth and functions <= max_depth


def close_over(seeds, max_depth: int, step_cap: int = 200000):
    """Every type reachable from `seeds`, and every edge, within the depth bound.

    Returns (types, edges) where edges is a list of (source, operator, target).
    """
    seen, queue, edges = set(), [], []
    for typ in seeds:
        if within(typ, max_depth) and typ not in seen:
            seen.add(typ)
            queue.append(typ)

    steps = 0
    while queue and steps < step_cap:
        src = queue.pop()
        for operator, successors_of in OPERATOR_REACHABLE_TYPE_MAP.items():
            try:
                successors = successors_of(src)
            except Exception:
                continue
            for dst in successors or ():
                steps += 1
                if not within(dst, max_depth):
                    continue
                edges.append((src, operator, dst))
                if dst not in seen:
                    seen.add(dst)
                    queue.append(dst)
    return seen, edges, steps >= step_cap


def primitive_seeds():
    return [NumberPType(), StringPType(), BooleanPType()]


def builtin_seeds():
    """The primitives plus the type of every name PLDI puts in scope by default."""
    seeds = primitive_seeds()
    globals_ = getattr(types_ts, "GLOBAL_OBJECTS", None)
    if globals_ is None:
        from typesafe_llm.parser.parser_ts import GLOBAL_OBJECTS as globals_
    for entry in globals_.values():
        typ = entry[0] if isinstance(entry, tuple) else entry
        seeds.append(typ)
    return seeds, len(globals_)


def subset_seeds():
    """The types our subset declares: three primitives and arrays of them."""
    from typesafe_llm.parser.types_ts import ArrayPType

    base = primitive_seeds()
    return base + [ArrayPType(t) for t in base]


def report(name: str, seeds, max_depth: int) -> dict:
    types, edges, capped = close_over(seeds, max_depth)
    by_operator = collections.Counter(op for _, op, _ in edges)
    distinct = {(str(a), op, str(b)) for a, op, b in edges}
    print(f"\n{name}, nesting depth <= {max_depth}")
    print(f"  types                 {len(types)}")
    print(f"  edges (distinct)      {len(distinct)}")
    print(f"  avg out-degree        {len(distinct)/max(1,len(types)):.1f}")
    if capped:
        print("  WARNING: hit the step cap, counts are a lower bound")
    for operator, count in by_operator.most_common():
        print(f"      {operator:6} {count}")
    return {"types": len(types), "edges": len(distinct)}


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--max-depth", type=int, default=2)
    args = ap.parse_args()

    builtins_, n_globals = builtin_seeds()
    print(f"PLDI puts {n_globals} names in scope by default")

    for depth in range(args.max_depth + 1):
        full = report("PLDI built-in environment", builtins_, depth)
        ours = report("our subset (primitives + arrays)", subset_seeds(), depth)
        if full["types"]:
            print(f"\n  subset / full : types {ours['types']/full['types']:.0%}, "
                  f"edges {ours['edges']/max(1,full['edges']):.0%}")


if __name__ == "__main__":
    main()
