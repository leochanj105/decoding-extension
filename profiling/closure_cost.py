"""Cost of building the type-reachability relation bottom-up, once.

Three sizes of the same idea, measured rather than estimated:

  per-goal   for one required type G, which types reach G (one reverse search)
  all-pairs  for every type, which types reach it (the full relation)
  state-level for each goal, which (parser state kind, type) pairs reach it

Costs are counted in the same units as the top-down profiler: an "operator try" is
one call to one of the checker's edge functions on one type, and an "edge step" is
one traversal of one edge.

    python3 closure_cost.py --states 1001

Needs Python 3.11 and: frozenlist regex termcolor frozendict tokenizers
"""

import argparse
import csv
import os
import sys
import time
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.join(HERE, "..", "type-constrained-code-generation")
sys.path.insert(0, REPO)
CORPUS_DIR = os.path.join(HERE, "corpus")
END_MARKER = "```"

from typesafe_llm.parser import types_ts                          # noqa: E402
from typesafe_llm.parser.parser_base import DerivableTypeMixin     # noqa: E402
from typesafe_llm.parser.parser_ts import (                        # noqa: E402
    custom_end_initial_state, incremental_ts_parse,
)

EDGE_FNS = dict(types_ts.OPERATOR_REACHABLE_TYPE_MAP)
MAX_CHAIN = 5   # the checker's own search budget


def nodes_of(state):
    yield state
    active = getattr(state, "active", None)
    if isinstance(active, (list, tuple)):
        kids = [c for c in active if hasattr(c, "identifiers")]
    elif hasattr(active, "identifiers"):
        kids = [active]
    else:
        kids = []
    for kid in kids:
        yield from nodes_of(kid)


def build_graph(seeds):
    """Every type constructible from the seeds, and the edges between them."""
    cap = (0, 0)
    for t in seeds:
        d = t.nesting_depth
        cap = (max(cap[0], d[0]), max(cap[1], d[1]))
    nodes, frontier = set(seeds), set(seeds)
    forward = defaultdict(set)
    tries = produced = 0
    for _ in range(MAX_CHAIN):
        nxt = set()
        for t in frontier:
            for fn in EDGE_FNS.values():
                tries += 1
                try:
                    out = fn(t)
                except Exception:
                    continue
                for c in out:
                    produced += 1
                    d = c.nesting_depth
                    if d[0] <= cap[0] and d[1] <= cap[1]:
                        forward[t].add(c)
                        if c not in nodes:
                            nodes.add(c)
                            nxt.add(c)
        frontier = nxt
        if not frontier:
            break
    return nodes, forward, tries, produced


def reverse_of(forward):
    rev = defaultdict(set)
    for src, dsts in forward.items():
        for dst in dsts:
            rev[dst].add(src)
    return rev


def reverse_reach(goal, rev):
    """Types that reach goal. Returns (set, edge steps taken)."""
    seen, queue, steps = {goal}, [goal], 0
    while queue:
        cur = queue.pop()
        for pred in rev.get(cur, ()):
            steps += 1
            if pred not in seen:
                seen.add(pred)
                queue.append(pred)
    return seen, steps


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("program", nargs="?")
    ap.add_argument("--states", type=int, default=0)
    ap.add_argument("--results", default="results_quick.csv")
    ap.add_argument("--tokenizer", default=os.path.join(HERE, "tok.json"))
    ap.add_argument("--at-token", type=int, default=60)
    ap.add_argument("--queries", default=os.path.join(HERE, "reach_heavy_queries.csv"))
    args = ap.parse_args()

    path = args.program
    if not path:
        best = None
        for row in csv.DictReader(open(os.path.join(HERE, args.results))):
            if row["status"] != "ok" or not row["states"]:
                continue
            if int(row["states"]) < args.states:
                continue
            if best is None or float(row["seconds"]) > float(best["seconds"]):
                best = row
        path = os.path.join(CORPUS_DIR, best["path"])
        print(f"picked {best['path']}")

    text = open(path).read().rstrip("\n")
    from tokenizers import Tokenizer
    tok = Tokenizer.from_file(args.tokenizer)
    pieces = [p for p in (tok.decode([i]) for i in tok.encode(text).ids) if p]

    state = custom_end_initial_state(END_MARKER, {})
    for piece in pieces[: args.at_token]:
        state = incremental_ts_parse(state, piece)
        if not state.active_states:
            break

    # scope, and the parser-state kinds that can produce a value
    symbols, kinds = {}, Counter()
    for top in state.active_states:
        for node in nodes_of(top):
            for name, entry in getattr(node, "identifiers", {}).items():
                symbols[name] = entry[0] if isinstance(entry, tuple) else entry
            if isinstance(node, DerivableTypeMixin):
                kinds[type(node).__name__] += 1
    seeds = set(symbols.values())

    t0 = time.perf_counter()
    nodes, forward, tries, produced = build_graph(seeds)
    build_s = time.perf_counter() - t0
    edges = sum(len(v) for v in forward.values())
    rev = reverse_of(forward)

    print(f"\nat token {args.at_token}: {len(symbols)} symbols, {len(seeds)} distinct types")
    print(f"graph: {len(nodes)} types, {edges} edges")
    print(f"build cost: {tries:,} operator tries, {produced:,} types produced, {build_s*1000:.1f} ms")

    # ---- per-goal
    goals_seen = []
    if os.path.exists(args.queries):
        goals_seen = [g for g, _ in Counter(
            r["goal_type"] for r in csv.DictReader(open(args.queries))
            if r["kind"] == "reachable").most_common()]
    by_name = {str(n): n for n in nodes}
    goals = [by_name[g] for g in goals_seen if g in by_name]

    print(f"\n--- per goal (one reverse search each) ---")
    print(f"  {'goal':<44}{'reach it':>10}{'edge steps':>12}{'ms':>8}")
    total_steps = 0
    for g in goals:
        t0 = time.perf_counter()
        reaching, steps = reverse_reach(g, rev)
        ms = (time.perf_counter() - t0) * 1000
        total_steps += steps
        print(f"  {str(g)[:43]:<44}{len(reaching):>10}{steps:>12}{ms:>8.2f}")
    print(f"  {'TOTAL for the goals this program used':<44}{'':>10}{total_steps:>12}")

    # ---- all pairs
    t0 = time.perf_counter()
    all_steps = 0
    relation = {}
    for n in nodes:
        reaching, steps = reverse_reach(n, rev)
        relation[n] = reaching
        all_steps += steps
    all_s = time.perf_counter() - t0
    pairs = sum(len(v) for v in relation.values())
    print(f"\n--- all pairs (every type as a goal) ---")
    print(f"  searches            : {len(nodes)}")
    print(f"  edge steps          : {all_steps:,}")
    print(f"  wall time           : {all_s*1000:.0f} ms")
    print(f"  relation size       : {pairs:,} pairs of {len(nodes)**2:,} possible "
          f"({100*pairs/len(nodes)**2:.0f}% dense)")
    print(f"  as a bit matrix     : {len(nodes)**2/8/1024:.1f} KB")

    # ---- what fraction TCD ever asked about
    if os.path.exists(args.queries):
        asked = {(r["source_type"], r["goal_type"])
                 for r in csv.DictReader(open(args.queries)) if r["kind"] == "reachable"}
        print(f"  pairs TCD ever asked: {len(asked)} "
              f"({100*len(asked)/max(pairs,1):.2f}% of the relation)")

    # ---- state level
    print(f"\n--- state level ---")
    print(f"  parser state kinds that produce a value: {len(kinds)}")
    for name, count in kinds.most_common(8):
        print(f"    {count:>6} live   {name}")
    print(f"  potential (state kind, type) pairs: {len(kinds)} x {len(nodes)} = "
          f"{len(kinds)*len(nodes):,}")
    reach_sizes = [len(relation[g]) for g in goals] or [0]
    print(f"  for one goal, (state kind, reaching type) pairs: "
          f"{len(kinds)} x {min(reach_sizes)}-{max(reach_sizes)} = "
          f"{len(kinds)*min(reach_sizes):,}-{len(kinds)*max(reach_sizes):,}")


if __name__ == "__main__":
    main()
