"""Profile TCD's top-down type-reachability queries, and its dynamic vocabulary.

Each time the checker asks "can a currently derivable type T still reach the
required type G?", it calls types_ts.reachable(T, G, ...). This records every such
query, the search work it costs, how often queries repeat, and how much is reused
across decoding steps. It also records the live symbol environment per step.

Nothing in the paper's code is edited. Instrumentation is installed by wrapping:
  - types_ts.reachable / any_reachable        (the queries themselves)
  - OPERATOR_REACHABLE_TYPE_MAP entries       (the edge relation: type -> types)
  - member_access_reachable_types             (optional-chaining edges)
  - GLOBAL_REACHABLE_CACHE                    (counting dict, for true hit/miss)

parser_ts imports reachable/any_reachable by name, so both modules are patched.

    python3 profile_reachability.py corpus/programs/.../foo.ts
    python3 profile_reachability.py --states 1001 --max-queries 200000

Writes <out>_queries.csv (one row per query) and <out>_steps.csv (per decoding step).

Needs Python 3.11 and: frozenlist regex termcolor frozendict tokenizers
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

from typesafe_llm.parser import parser_ts, types_ts            # noqa: E402
from typesafe_llm.parser.parser_ts import (                    # noqa: E402
    custom_end_initial_state,
    incremental_ts_parse,
)

# ---------------------------------------------------------------- instrumentation

class CountingCache(dict):
    """The paper's GLOBAL_REACHABLE_CACHE, with hit/miss counters."""
    def __init__(self, *a, **k):
        super().__init__(*a, **k)
        self.hits = self.misses = self.writes = 0
    def __contains__(self, key):
        present = super().__contains__(key)
        if present:
            self.hits += 1
        else:
            self.misses += 1
        return present
    def __setitem__(self, key, value):
        self.writes += 1
        super().__setitem__(key, value)


WORK = {"expansions": 0, "edges": 0}


def wrap_edge_fn(fn):
    """Count one expansion per call, and one edge per type returned."""
    def wrapped(typ):
        WORK["expansions"] += 1
        out = fn(typ)
        WORK["edges"] += len(out)
        return out
    wrapped.__wrapped__ = fn
    return wrapped


def install():
    cache = CountingCache()
    types_ts.GLOBAL_REACHABLE_CACHE = cache
    types_ts.OPERATOR_REACHABLE_TYPE_MAP = {
        op: wrap_edge_fn(fn) for op, fn in types_ts.OPERATOR_REACHABLE_TYPE_MAP.items()
    }
    types_ts.member_access_reachable_types = wrap_edge_fn(
        types_ts.member_access_reachable_types
    )
    return cache


QUERIES = []          # one record per reachable() call
STATE = {"step": -1, "char": 0, "budget": 0}


def patch_queries(cache, max_queries):
    original_reachable = types_ts.reachable
    original_any = types_ts.any_reachable

    def record(t, goal, min_p, max_p, in_array, in_nested, in_pattern, max_steps,
               kind, n_sources):
        e0, x0 = WORK["expansions"], WORK["edges"]
        h0, m0, w0 = cache.hits, cache.misses, cache.writes
        t0 = time.perf_counter()
        yield              # caller runs the real query here
        elapsed = time.perf_counter() - t0
        if len(QUERIES) < max_queries:
            QUERIES.append({
                "step": STATE["step"], "char": STATE["char"], "kind": kind,
                "source_type": str(t), "goal_type": str(goal),
                "n_sources": n_sources,
                "min_prec": f"{min_p[0]}{min_p[1][0]}", "max_prec": f"{max_p[0]}{max_p[1][0]}",
                "in_array": len(in_array), "in_nested": len(in_nested),
                "in_pattern": len(in_pattern), "max_steps": max_steps,
                "expansions": WORK["expansions"] - e0,
                "edges": WORK["edges"] - x0,
                "cache_hits": cache.hits - h0, "cache_misses": cache.misses - m0,
                "cache_writes": cache.writes - w0,
                "seconds": round(elapsed, 7),
            })

    def reachable(t, goal_t, min_p, max_p, in_array, in_nested, in_pattern,
                  max_steps=5, max_depth=(0, 0)):
        gen = record(t, goal_t, min_p, max_p, in_array, in_nested, in_pattern,
                     max_steps, "reachable", 1)
        next(gen)
        try:
            return original_reachable(t, goal_t, min_p, max_p, in_array, in_nested,
                                      in_pattern, max_steps, max_depth)
        finally:
            next(gen, None)

    def any_reachable(typs, goal_t, min_p, max_p, in_array, in_nested, in_pattern,
                      max_steps=5, max_depth=(0, 0)):
        # any_reachable calls reachable per source type; those are recorded too.
        # This row records the batch, so batches can be told from single queries.
        gen = record(next(iter(typs), None), goal_t, min_p, max_p, in_array, in_nested,
                     in_pattern, max_steps, "any_reachable", len(typs))
        next(gen)
        try:
            return original_any(typs, goal_t, min_p, max_p, in_array, in_nested,
                                in_pattern, max_steps, max_depth)
        finally:
            next(gen, None)

    for module in (types_ts, parser_ts):
        module.reachable = reachable
        module.any_reachable = any_reachable


# ---------------------------------------------------------------- environment


def _all_nodes(state):
    """Every state in a reading tree, so nested scopes are not missed.

    A declaration inside a function body lives in a nested state; the top-level
    state's identifiers dict only holds the global scope, so reading just that one
    reports a constant environment no matter what the program declares.
    """
    yield state
    active = getattr(state, "active", None)
    if isinstance(active, (list, tuple)):
        kids = [c for c in active if hasattr(c, "identifiers")]
    elif hasattr(active, "identifiers"):
        kids = [active]
    else:
        kids = []
    for kid in kids:
        yield from _all_nodes(kid)


def live_symbols(parser_state):
    """Names in scope at this point, mapped to their type, unioned over readings."""
    symbols = {}
    for top in parser_state.active_states:
        for node in _all_nodes(top):
            for name, entry in getattr(node, "identifiers", {}).items():
                typ = entry[0] if isinstance(entry, tuple) else entry
                symbols[name] = str(typ)
    return symbols


def trie_nodes(names):
    """Nodes in a prefix trie over these names -- the concrete candidate space."""
    root = {}
    for name in names:
        node = root
        for ch in name:
            node = node.setdefault(ch, {})
    total = 0
    stack = [root]
    while stack:
        node = stack.pop()
        total += len(node)
        stack.extend(node.values())
    return total


# ---------------------------------------------------------------- main

def pick_program(args):
    if args.program:
        return args.program
    best = None
    for row in csv.DictReader(open(os.path.join(HERE, args.results))):
        if row["status"] != "ok" or not row["states"]:
            continue
        if int(row["states"]) < args.states:
            continue
        if best is None or float(row["seconds"]) > float(best["seconds"]):
            best = row
    if not best:
        raise SystemExit(f"no program with >= {args.states} states")
    print(f"picked {best['path']} ({best['states']} top-level states)")
    return os.path.join(CORPUS_DIR, best["path"])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("program", nargs="?")
    ap.add_argument("--states", type=int, default=0)
    ap.add_argument("--results", default="results_quick.csv")
    ap.add_argument("--tokenizer", default=os.path.join(HERE, "tok.json"),
                    help="tokenizer.json defining decoding-step boundaries")
    ap.add_argument("--max-queries", type=int, default=400000)
    ap.add_argument("--max-tokens", type=int, default=0, help="0 = whole program")
    ap.add_argument("--out", default=os.path.join(HERE, "reach"))
    args = ap.parse_args()

    path = pick_program(args)
    text = open(path).read().rstrip("\n")

    from tokenizers import Tokenizer
    tok = Tokenizer.from_file(args.tokenizer)
    ids = tok.encode(text).ids
    pieces = [tok.decode([i]) for i in ids]
    # keep only what reconstructs the text, so steps line up with real decoding
    pieces = [p for p in pieces if p]
    if args.max_tokens:
        pieces = pieces[: args.max_tokens]
    print(f"{os.path.basename(path)}: {len(text)} chars, {len(pieces)} tokens")

    cache = install()
    patch_queries(cache, args.max_queries)

    parser_state = custom_end_initial_state(END_MARKER, {})
    steps = []
    previous = {}
    started = time.perf_counter()

    for step, piece in enumerate(pieces):
        STATE["step"] = step
        q0 = len(QUERIES)
        e0, x0 = WORK["expansions"], WORK["edges"]
        h0, m0 = cache.hits, cache.misses
        t0 = time.perf_counter()
        parser_state = incremental_ts_parse(parser_state, piece)
        elapsed = time.perf_counter() - t0
        if not parser_state.active_states:
            print(f"rejected at token {step} {piece!r}")
            break
        STATE["char"] += len(piece)

        symbols = live_symbols(parser_state)
        by_type = Counter(symbols.values())
        added = set(symbols) - set(previous)
        removed = set(previous) - set(symbols)
        goals = {q["goal_type"] for q in QUERIES[q0:]}
        steps.append({
            "step": step, "token": repr(piece), "chars_done": STATE["char"],
            "seconds": round(elapsed, 5),
            "queries": len(QUERIES) - q0,
            "expansions": WORK["expansions"] - e0, "edges": WORK["edges"] - x0,
            "cache_hits": cache.hits - h0, "cache_misses": cache.misses - m0,
            "live_symbols": len(symbols), "distinct_symbol_types": len(by_type),
            "max_symbols_one_type": max(by_type.values()) if by_type else 0,
            "symbols_added": len(added), "symbols_removed": len(removed),
            "trie_nodes": trie_nodes(symbols),
            "distinct_goals": len(goals),
        })
        previous = symbols

    qf, sf = args.out + "_queries.csv", args.out + "_steps.csv"
    if QUERIES:
        with open(qf, "w", newline="") as h:
            w = csv.DictWriter(h, fieldnames=list(QUERIES[0].keys())); w.writeheader()
            w.writerows(QUERIES)
    with open(sf, "w", newline="") as h:
        w = csv.DictWriter(h, fieldnames=list(steps[0].keys())); w.writeheader()
        w.writerows(steps)

    print(f"\nreplayed {len(steps)} tokens in {time.perf_counter()-started:.1f}s")
    print(f"  queries recorded : {len(QUERIES):,}"
          f"{'  (capped)' if len(QUERIES) >= args.max_queries else ''}")
    print(f"  expansions       : {WORK['expansions']:,}")
    print(f"  edges generated  : {WORK['edges']:,}")
    print(f"  cache hits/misses: {cache.hits:,} / {cache.misses:,}")
    print(f"  cache entries    : {len(cache):,}")
    print(f"  -> {os.path.basename(qf)}, {os.path.basename(sf)}")


if __name__ == "__main__":
    main()
