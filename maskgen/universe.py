"""The type universe and its reachability table.

For the fragment in FRAGMENT.md the universe is finite and known before generation
starts: every type is built from six base types, function signatures draw their
parameters and results from those, and the member tables are fixed. Nothing here
depends on what a program declares. So the whole relation is computed once, offline,
and every later question is a bit test.

Two directions are stored, because the mask needs the second:

    reaches(a, b)        can a value of type a become one of type b
    sources_reaching(b)  every type that can become b          <- what a mask needs

Types are interned to small integers and sets of types are Python ints used as
bitsets, so a query is a shift and an AND.

Measured for the fragment at nesting depth 2: 125 types, 750 edges, 57.6% of pairs
reachable, built in a few tens of milliseconds.

The one thing to get right here is that **calling a function requires its arguments
to be writable**. Member access yields a method's type freely, but using it means
supplying every parameter, and the fragment has no arrow functions. So
`string[].reduce` cannot fire: its callback type is not obtainable. Leaving that
ungated makes every type reach every other and the filter vacuous. Gated, `number`
does not reach `number[]` -- nothing turns a number into an array of numbers -- while
`number` does reach `string[]` via `(5 + "").split(",")`.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field

from maskgen.type_graph import close_over, close_with_argument_gate, six_types


@dataclass
class TypeUniverse:
    """Interned types plus the closed reachability relation over them."""

    types: list = field(default_factory=list)          # id -> type object
    ids: dict = field(default_factory=dict)            # type object -> id
    edges: list = field(default_factory=list)          # (src id, operator, dst id)
    reach: list = field(default_factory=list)          # id -> bitset of ids it reaches
    inverse: list = field(default_factory=list)        # id -> bitset of ids reaching it
    rounds: int = 0
    build_seconds: float = 0.0

    # ---------------------------------------------------------------- building

    @classmethod
    def for_fragment(cls, max_depth: int = 2, gated: bool = True) -> "TypeUniverse":
        """The fragment's universe.

        `gated` is the real model: a call fires only when every argument can be
        written. Without it the graph is 100% dense and the filter looks vacuous;
        with it, number does not reach number[] but does reach string[].
        """
        start = time.perf_counter()
        universe = cls()
        if gated:
            found, edge_set, _rounds = close_with_argument_gate(max_depth)
            edges = list(edge_set)
        else:
            found, edges, capped = close_over(six_types(), max_depth, mode="fragment")
            if capped:
                raise RuntimeError("closure hit its step cap; universe is not closed")

        # Intern on the type objects, not their strings. A type and its
        # optional-parameter spelling compare equal but stringify differently, so a
        # string key would split one type into two and lose edges.
        for typ in sorted(found, key=str):
            universe.ids[typ] = len(universe.types)
            universe.types.append(typ)

        seen_edges = set()
        for src, operator, dst in edges:
            a, b = universe.ids.get(src), universe.ids.get(dst)
            if a is None or b is None:
                continue
            if (a, operator, b) in seen_edges:
                continue
            seen_edges.add((a, operator, b))
            universe.edges.append((a, operator, b))

        universe._close()
        universe.build_seconds = time.perf_counter() - start
        return universe

    def _close(self) -> None:
        """Transitive, reflexive closure of the edge relation.

        Reflexive because a value of type T is already usable where T is wanted;
        the fragment has no subtyping, so assignability is equality.
        """
        n = len(self.types)
        step = [1 << i for i in range(n)]              # reflexive seed
        for a, _operator, b in self.edges:
            step[a] |= 1 << b

        self.rounds = 0
        changed = True
        while changed:
            changed = False
            self.rounds += 1
            for i in range(n):
                acc = step[i]
                rest = acc
                while rest:
                    j = (rest & -rest).bit_length() - 1
                    rest &= rest - 1
                    acc |= step[j]
                if acc != step[i]:
                    step[i] = acc
                    changed = True
        self.reach = step

        inverse = [0] * n
        for i in range(n):
            rest = self.reach[i]
            while rest:
                j = (rest & -rest).bit_length() - 1
                rest &= rest - 1
                inverse[j] |= 1 << i
        self.inverse = inverse

    # ---------------------------------------------------------------- querying

    def __len__(self) -> int:
        return len(self.types)

    def id_of(self, typ) -> int | None:
        return self.ids.get(typ)

    def reaches(self, source: int, goal: int) -> bool:
        return bool(self.reach[source] >> goal & 1)

    def sources_reaching(self, goal: int) -> int:
        """Bitset of every type usable where `goal` is required.

        This is the context filter's output before any syntactic restriction, and
        the only thing the per-type masks are indexed by.
        """
        return self.inverse[goal]

    def members_of(self, bitset: int) -> list[int]:
        out = []
        while bitset:
            out.append((bitset & -bitset).bit_length() - 1)
            bitset &= bitset - 1
        return out

    def describe(self) -> str:
        pairs = sum(bin(r).count("1") for r in self.reach)
        n = len(self.types)
        return (
            f"{n} types, {len(self.edges)} edges, closure in {self.rounds} rounds, "
            f"{pairs}/{n*n} reachable pairs ({100*pairs/(n*n):.0f}% dense), "
            f"{2*n*((n+7)//8)} bytes, built in {self.build_seconds*1000:.0f} ms"
        )
