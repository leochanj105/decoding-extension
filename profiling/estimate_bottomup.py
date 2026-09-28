"""Estimate a bottom-up alternative to TCD's top-down reachability queries.

TCD asks, one query at a time, "can type T still reach goal G?". The alternative is
to invert it: once the type environment and the goal G are known, compute *once* the
set of all source types that can reach G (reverse reachability from G), then answer
every later query about that G by set membership.

This replays a program, and at each point where the environment or goal changes it
snapshots the type graph and builds that set, charging the construction cost once.
It then compares:

  - total search work, top-down vs bottom-up
  - memory (size of the materialized relation)
  - what fraction of the materialized set TCD actually asked about
  - how long each (environment, goal) pair stays valid
  - how early the construction could have started, versus when TCD first needs it
    (the earlier it can start, the more it can overlap a GPU forward pass)

Approximation, stated plainly: the bottom-up set is built over *types* only. TCD's
real cache key is a 10-tuple that also carries operator precedence, array/pattern
context and a step budget. So the materialized set is an over-approximation of what
a context-sensitive query would allow, and the script separately reports how many
distinct (T,G) pairs TCD queried versus how many distinct full keys -- i.e. how much
of TCD's work is context refinement rather than type reachability.

    python3 estimate_bottomup.py corpus/programs/.../foo.ts
    python3 estimate_bottomup.py --states 1001

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

from typesafe_llm.parser import parser_ts, types_ts              # noqa: E402
from typesafe_llm.parser.parser_ts import (                      # noqa: E402
    custom_end_initial_state,
    incremental_ts_parse,
)

MAX_STEPS = 5          # TCD's default search budget
UNIVERSE_CAP = 20000   # refuse to explore a silly number of types

# Unwrapped edge relation, captured before any instrumentation.
EDGE_FNS = dict(types_ts.OPERATOR_REACHABLE_TYPE_MAP)
MEMBER_ACCESS = types_ts.member_access_reachable_types

# ------------------------------------------------------------------ type graph

def successors(typ, depth_cap, work):
    """One forward step: every type reachable from typ by one operator or access."""
    out = set()
    for fn in EDGE_FNS.values():
        work["expansions"] += 1
        try:
            produced = fn(typ)
        except Exception:
            continue
        for candidate in produced:
            work["edges"] += 1
            nd = candidate.nesting_depth
            if nd[0] <= depth_cap[0] and nd[1] <= depth_cap[1]:
                out.add(candidate)
    return out


def build_graph(seeds, goal, work):
    """Close over forward edges from the seeds and the goal, to MAX_STEPS depth.

    Returns (nodes, forward adjacency). The depth cap mirrors what reachable()
    computes: the elementwise max nesting depth of the types involved.
    """
    depth_cap = (0, 0)
    for typ in list(seeds) + [goal]:
        nd = typ.nesting_depth
        depth_cap = (max(depth_cap[0], nd[0]), max(depth_cap[1], nd[1]))

    nodes = set(seeds) | {goal}
    frontier = set(nodes)
    adjacency = defaultdict(set)
    for _ in range(MAX_STEPS):
        nxt = set()
        for typ in frontier:
            for succ in successors(typ, depth_cap, work):
                adjacency[typ].add(succ)
                if succ not in nodes:
                    nodes.add(succ)
                    nxt.add(succ)
        frontier = nxt
        if not frontier or len(nodes) > UNIVERSE_CAP:
            break
    return nodes, adjacency


def reverse_reachable(goal, nodes, adjacency, work):
    """All types that can reach the goal: BFS over reversed edges."""
    reverse = defaultdict(set)
    for src, dsts in adjacency.items():
        for dst in dsts:
            reverse[dst].add(src)
    found = {goal}
    queue = [goal]
    while queue:
        current = queue.pop()
        for pred in reverse.get(current, ()):
            work["edges"] += 1
            if pred not in found:
                found.add(pred)
                queue.append(pred)
    return found


# ------------------------------------------------------------------ replay


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


def env_types(parser_state):
    """The types of the symbols currently in scope, as real type objects."""
    types = {}
    for top in parser_state.active_states:
        for node in _all_nodes(top):
            for name, entry in getattr(node, "identifiers", {}).items():
                typ = entry[0] if isinstance(entry, tuple) else entry
                types[name] = typ
    return types


TD_WORK = {"expansions": 0, "edges": 0}


def wrap_edges_for_replay():
    """Count the edge relation TCD traverses top-down, without altering behaviour."""
    def wrap(fn):
        def wrapped(typ):
            TD_WORK["expansions"] += 1
            out = fn(typ)
            TD_WORK["edges"] += len(out)
            return out
        return wrapped
    types_ts.OPERATOR_REACHABLE_TYPE_MAP = {
        op: wrap(fn) for op, fn in EDGE_FNS.items()
    }
    types_ts.member_access_reachable_types = wrap(MEMBER_ACCESS)


def restore_edges():
    """Put the originals back, so the bottom-up build is counted separately."""
    types_ts.OPERATOR_REACHABLE_TYPE_MAP = dict(EDGE_FNS)
    types_ts.member_access_reachable_types = MEMBER_ACCESS


def install_query_log():
    """Record every top-down query, with its source type, goal and full key shape."""
    log = []
    original = types_ts.reachable

    def reachable(t, goal_t, min_p, max_p, in_array, in_nested, in_pattern,
                  max_steps=5, max_depth=(0, 0)):
        log.append((t, goal_t, (min_p, max_p, len(in_array), len(in_nested),
                                len(in_pattern), max_steps)))
        return original(t, goal_t, min_p, max_p, in_array, in_nested, in_pattern,
                        max_steps, max_depth)

    for module in (types_ts, parser_ts):
        module.reachable = reachable
    return log


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
    print(f"picked {best['path']}")
    return os.path.join(CORPUS_DIR, best["path"])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("program", nargs="?")
    ap.add_argument("--states", type=int, default=0)
    ap.add_argument("--results", default="results_quick.csv")
    ap.add_argument("--tokenizer", default=os.path.join(HERE, "tok.json"))
    ap.add_argument("--max-tokens", type=int, default=0)
    ap.add_argument("--out", default=os.path.join(HERE, "reach_bottomup.csv"))
    args = ap.parse_args()

    path = pick_program(args)
    text = open(path).read().rstrip("\n")
    from tokenizers import Tokenizer
    tok = Tokenizer.from_file(args.tokenizer)
    pieces = [p for p in (tok.decode([i]) for i in tok.encode(text).ids) if p]
    if args.max_tokens:
        pieces = pieces[: args.max_tokens]
    print(f"{os.path.basename(path)}: {len(text)} chars, {len(pieces)} tokens")

    query_log = install_query_log()
    wrap_edges_for_replay()
    parser_state = custom_end_initial_state(END_MARKER, {})

    # Per step: the environment, and the queries TCD issued.
    per_step = []
    for step, piece in enumerate(pieces):
        before = len(query_log)
        parser_state = incremental_ts_parse(parser_state, piece)
        if not parser_state.active_states:
            print(f"rejected at token {step}")
            break
        per_step.append({
            "step": step,
            "env": env_types(parser_state),
            "queries": query_log[before:],
        })

    restore_edges()

    # An epoch is a run of steps whose environment type-set is unchanged.
    # Within an epoch, each goal's materialized set can be built once and reused.
    epochs = []
    for record in per_step:
        signature = frozenset(record["env"].values())
        if epochs and epochs[-1]["signature"] == signature:
            epochs[-1]["steps"].append(record)
        else:
            epochs.append({"signature": signature, "steps": [record]})
    print(f"{len(per_step)} steps -> {len(epochs)} environment epochs")

    rows = []
    td_expansions = td_edges = 0
    for epoch_id, epoch in enumerate(epochs):
        seeds = set(epoch["signature"])
        first_step = epoch["steps"][0]["step"]
        last_step = epoch["steps"][-1]["step"]

        # goals TCD actually asked about during this epoch, and when each first appeared
        goal_first_step, goal_sources, goal_keys, goal_query_count = {}, defaultdict(set), defaultdict(set), Counter()
        for record in epoch["steps"]:
            for (source, goal, shape) in record["queries"]:
                goal_first_step.setdefault(goal, record["step"])
                goal_sources[goal].add(source)
                goal_keys[goal].add((source, shape))
                goal_query_count[goal] += 1
                td_expansions += 1

        for goal, first_query_step in goal_first_step.items():
            work = {"expansions": 0, "edges": 0}
            t0 = time.perf_counter()
            nodes, adjacency = build_graph(seeds, goal, work)
            materialized = reverse_reachable(goal, nodes, adjacency, work)
            build_seconds = time.perf_counter() - t0
            queried = goal_sources[goal]
            rows.append({
                "epoch": epoch_id,
                "epoch_first_step": first_step,
                "epoch_last_step": last_step,
                "epoch_len_steps": last_step - first_step + 1,
                "env_types": len(seeds),
                "goal": str(goal),
                "first_query_step": first_query_step,
                "lead_time_steps": first_query_step - first_step,
                "td_queries": goal_query_count[goal],
                "td_distinct_pairs": len(queried),
                "td_distinct_keys": len(goal_keys[goal]),
                "bu_graph_nodes": len(nodes),
                "bu_graph_edges": sum(len(v) for v in adjacency.values()),
                "bu_materialized": len(materialized),
                "bu_expansions": work["expansions"],
                "bu_edges": work["edges"],
                "bu_seconds": round(build_seconds, 5),
                "queried_in_materialized": len(queried & materialized),
                "frac_materialized_used": round(
                    len(queried & materialized) / max(len(materialized), 1), 4),
            })

    with open(args.out, "w", newline="") as h:
        w = csv.DictWriter(h, fieldnames=list(rows[0].keys())); w.writeheader()
        w.writerows(rows)

    # ---- summary
    tot_td_queries = sum(r["td_queries"] for r in rows)
    tot_pairs = sum(r["td_distinct_pairs"] for r in rows)
    tot_keys = sum(r["td_distinct_keys"] for r in rows)
    tot_bu_exp = sum(r["bu_expansions"] for r in rows)
    tot_bu_edges = sum(r["bu_edges"] for r in rows)
    tot_mat = sum(r["bu_materialized"] for r in rows)
    tot_used = sum(r["queried_in_materialized"] for r in rows)
    lead = sorted(r["lead_time_steps"] for r in rows)
    life = sorted(r["epoch_len_steps"] for r in rows)

    print(f"\n=== top-down (what TCD does) ===")
    print(f"  queries issued                : {tot_td_queries:,}")
    print(f"  expansions                    : {TD_WORK['expansions']:,}")
    print(f"  edges touched                 : {TD_WORK['edges']:,}")
    print(f"  distinct (T,G) pairs          : {tot_pairs:,}")
    print(f"  distinct full keys (T,G,ctx)  : {tot_keys:,}")
    print(f"  duplicate-query factor        : {tot_td_queries/max(tot_pairs,1):.1f}x "
          f"over (T,G); {tot_td_queries/max(tot_keys,1):.1f}x over full keys")
    print(f"\n=== bottom-up (hypothetical, once per epoch+goal) ===")
    print(f"  constructions                 : {len(rows):,}")
    print(f"  expansions                    : {tot_bu_exp:,}")
    print(f"  edges touched                 : {tot_bu_edges:,}")
    print(f"  materialized entries (memory) : {tot_mat:,}")
    print(f"  of those, actually queried    : {tot_used:,} "
          f"({100*tot_used/max(tot_mat,1):.1f}%)")
    print(f"\n=== head to head ===")
    print(f"  expansions  bottom-up / top-down: "
          f"{tot_bu_exp:,} / {TD_WORK['expansions']:,} = "
          f"{tot_bu_exp/max(TD_WORK['expansions'],1):.2f}x")
    print(f"  edges       bottom-up / top-down: "
          f"{tot_bu_edges:,} / {TD_WORK['edges']:,} = "
          f"{tot_bu_edges/max(TD_WORK['edges'],1):.2f}x")

    print(f"\n=== reuse and overlap ===")
    print(f"  epoch lifetime (steps)        : median {life[len(life)//2]}, max {life[-1]}")
    print(f"  lead time, env+goal known before first query (steps): "
          f"median {lead[len(lead)//2]}, max {lead[-1]}")
    print(f"  constructions with lead > 0   : {sum(1 for l in lead if l > 0)}/{len(lead)}")
    print(f"\n  -> {os.path.basename(args.out)}")


if __name__ == "__main__":
    main()
