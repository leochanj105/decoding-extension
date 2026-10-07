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
import itertools
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.join(HERE, "..", "type-constrained-code-generation")
if REPO not in sys.path:
    sys.path.insert(0, REPO)

from typesafe_llm.parser import types_ts  # noqa: E402
from typesafe_llm.parser.types_ts import (  # noqa: E402
    OPERATOR_REACHABLE_TYPE_MAP,
    ArrayPType,
    BooleanPType,
    FunctionPType,
    NumberPType,
    StringPType,
    UnionPType,
)


def base_types():
    return [NumberPType(), StringPType(), BooleanPType()]


def six_types():
    """The fragment's types: three primitives and arrays of them."""
    base = base_types()
    return base + [ArrayPType(t) for t in base]


_SIX_NAMES = {str(t) for t in six_types()}


def types_mentioned(typ, seen=None) -> set:
    """Every atomic type name appearing anywhere inside `typ`."""
    seen = seen if seen is not None else set()
    if id(typ) in seen:
        return set()
    seen.add(id(typ))
    if isinstance(typ, ArrayPType):
        return types_mentioned(typ.element_type, seen)
    if isinstance(typ, FunctionPType):
        out = set()
        for param in getattr(typ, "call_signature", ()) or ():
            out |= types_mentioned(param, seen)
        return out | types_mentioned(typ.return_type, seen)
    return {str(typ)}


def _fn(params, ret):
    return FunctionPType(call_signature=tuple(params), return_type=ret)


def monomorphic_higher_order(typ) -> set:
    """Generic array methods instantiated at concrete types, so no type parameters.

    `number[].map` with a `(number) => string` callback yields `string[]`. Nine
    instantiations per array type replace one generic signature, which is the same
    trick the fragment already uses to turn `push` into `(number) => number`.
    """
    if not isinstance(typ, ArrayPType):
        return set()
    elem = typ.element_type
    out = set()
    for result in base_types():
        out.add(_fn([_fn([elem], result)], ArrayPType(result)))        # map
        out.add(_fn([_fn([result, elem], result), result], result))    # reduce
    predicate = _fn([elem], BooleanPType())
    out.add(_fn([predicate], typ))                                    # filter
    out.add(_fn([predicate], BooleanPType()))                         # some / every
    return out


def narrow_to_six(typ) -> set:
    """Every instantiation of `typ` that mentions only the six types.

    Unions are narrowed to their in-six branches, so `split(string | RegExp)`
    becomes `split(string)`. That is the same move as monomorphising a generic: the
    real signature has a branch we can express, and keeping it preserves the edge
    while dropping the member would lose `string -> string[]` entirely.

    Returns an empty set when no branch survives.
    """
    if isinstance(typ, UnionPType):
        out = set()
        for branch in typ.types:
            out |= narrow_to_six(branch)
        return out
    if isinstance(typ, ArrayPType):
        return {ArrayPType(e) for e in narrow_to_six(typ.element_type)}
    if isinstance(typ, FunctionPType):
        params = []
        for param in getattr(typ, "call_signature", ()) or ():
            options = narrow_to_six(param)
            if not options:
                return set()
            params.append(sorted(options, key=str))
        returns = narrow_to_six(typ.return_type)
        if not returns:
            return set()
        out = set()
        for combo in itertools.product(*params) if params else [()]:
            for ret in returns:
                out.add(_fn(list(combo), ret))
        return out
    return {typ} if str(typ) in _SIX_NAMES else set()


def members_narrowed(typ) -> set:
    """Member types with unions narrowed to their in-six branches."""
    try:
        attributes = typ.attributes
    except Exception:
        return set()
    out = set()
    for entry in attributes.values():
        member = entry[0] if isinstance(entry, tuple) else entry
        out |= narrow_to_six(member)
    return out


def members_within_six(typ) -> set:
    """Member types, dropping any member whose signature leaves the six types.

    What this drops is what the fragment already excludes: the arrays' generic
    methods (typed with `T` and `any`) and string's RegExp and union members.
    """
    try:
        attributes = typ.attributes
    except Exception:
        return set()
    out = set()
    for entry in attributes.values():
        member = entry[0] if isinstance(entry, tuple) else entry
        if not (types_mentioned(member) - _SIX_NAMES):
            out.add(member)
    return out


def edge_table(mode: str) -> dict:
    """The eight edge functions, with member access varied by `mode`.

    full        PLDI's member tables as they are
    within-six  only members whose signatures stay inside the six types
    fragment    those, with unions narrowed to their in-six branches
    fragment+ho plus monomorphic instantiations of the generic array methods

    The higher-order methods are a separate mode because their edges are not
    static. `number[].map` reaches `string[]` only by supplying a `(number) =>
    string` callback, and the fragment has no arrow functions -- the only way to
    produce a function value is to name a `declare function`. So those edges exist
    only when a suitable function has been declared, which makes them
    environment-dependent. Adding them unconditionally makes the graph 100% dense
    and the type filter useless.
    """
    table = dict(OPERATOR_REACHABLE_TYPE_MAP)
    if mode == "full":
        return table
    if mode == "within-six":
        table["p.x"] = members_within_six
    elif mode == "fragment":
        table["p.x"] = members_narrowed
    elif mode == "fragment+ho":
        table["p.x"] = lambda t: members_narrowed(t) | monomorphic_higher_order(t)
    else:
        raise ValueError(mode)
    return table


def within(typ, max_depth: int) -> bool:
    try:
        arrays, functions = typ.nesting_depth
    except Exception:
        return False
    return arrays <= max_depth and functions <= max_depth


def close_over(seeds, max_depth: int, step_cap: int = 400000, mode: str = "full"):
    """Every type reachable from `seeds`, and every edge, within the depth bound.

    Returns (types, edges, capped) where edges is a list of (source, operator,
    target).
    """
    table = edge_table(mode)
    seen, queue, edges = set(), [], []
    for typ in seeds:
        if within(typ, max_depth) and typ not in seen:
            seen.add(typ)
            queue.append(typ)

    steps = 0
    while queue and steps < step_cap:
        src = queue.pop()
        for operator, successors_of in table.items():
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
    return base_types()


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
    return six_types()


def report(name: str, seeds, max_depth: int, mode: str = "full") -> dict:
    types, edges, capped = close_over(seeds, max_depth, mode=mode)
    by_operator = collections.Counter(op for _, op, _ in edges)
    # Deduplicate on the types themselves. Counting by str() overcounts: a type and
    # its optional-parameter spelling compare equal but stringify differently, e.g.
    # (v0: number) => number versus (v0: number?) => number.
    distinct = {(a, op, b) for a, op, b in edges}
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
        full = report("PLDI built-in environment", builtins_, depth, "full")
        strict = report("six types, members within the six only",
                        subset_seeds(), depth, "within-six")
        frag = report("THE FRAGMENT: unions narrowed to in-six branches",
                      subset_seeds(), depth, "fragment")
        ho = report("fragment + monomorphic higher-order methods (not static)",
                    subset_seeds(), depth, "fragment+ho")
        if not full["types"]:
            continue
        print(f"\n  share of PLDI's graph at depth <= {depth}")
        for label, got in (("within-six only", strict), ("the fragment", frag),
                           ("+ higher-order", ho)):
            print(f"      {label:18} types {got['types']/full['types']:.0%}, "
                  f"edges {got['edges']/max(1,full['edges']):.0%}")


if __name__ == "__main__":
    main()
