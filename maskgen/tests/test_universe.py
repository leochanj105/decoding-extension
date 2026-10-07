"""Is the reachability table actually closed, and does it say the right things?"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))

# importing this first is what puts the PLDI repo on sys.path
from maskgen.universe import TypeUniverse  # noqa: E402
from typesafe_llm.parser.types_ts import (  # noqa: E402
    ArrayPType,
    BooleanPType,
    NumberPType,
    StringPType,
)

_cache = {}


def universe(depth=2):
    if depth not in _cache:
        _cache[depth] = TypeUniverse.for_fragment(max_depth=depth)
    return _cache[depth]


def test_sizes_match_the_measurement():
    u = universe(2)
    print(f"      {u.describe()}")
    assert len(u) == 125, len(u)
    assert len(u.edges) == 766, len(u.edges)


def test_the_six_types_are_present():
    u = universe(2)
    for typ in (NumberPType(), StringPType(), BooleanPType(),
                ArrayPType(NumberPType()), ArrayPType(StringPType()),
                ArrayPType(BooleanPType())):
        assert u.id_of(typ) is not None, typ


def test_reflexive():
    u = universe(2)
    for i in range(len(u)):
        assert u.reaches(i, i), u.types[i]


def test_transitively_closed():
    """If a reaches b and b reaches c then a must reach c. Checks the fixpoint."""
    u = universe(2)
    for a in range(len(u)):
        for b in u.members_of(u.reach[a]):
            assert u.reach[a] & u.reach[b] == u.reach[b], (u.types[a], u.types[b])


def test_known_reachabilities():
    u = universe(2)
    num, string = u.id_of(NumberPType()), u.id_of(StringPType())
    boolean = u.id_of(BooleanPType())
    strings = u.id_of(ArrayPType(StringPType()))
    assert u.reaches(num, string), "number -> string via toString() or +"
    assert u.reaches(string, num), "string -> number via .length"
    # .split takes (string | RegExp); narrowing the union to its string branch is
    # what keeps this edge, and without it nothing turns a string into a string[].
    assert u.reaches(string, strings), "string -> string[] via .split()"
    assert u.reaches(num, boolean), "number -> boolean via =="
    assert u.reaches(strings, string), "string[] -> string via .join()"


def test_inverse_agrees_with_forward():
    u = universe(2)
    for goal in range(len(u)):
        for src in u.members_of(u.sources_reaching(goal)):
            assert u.reaches(src, goal), (u.types[src], u.types[goal])
    total_forward = sum(bin(r).count("1") for r in u.reach)
    total_inverse = sum(bin(r).count("1") for r in u.inverse)
    assert total_forward == total_inverse


def test_the_filter_actually_filters():
    """Not every type may reach every goal, or the filter selects nothing.

    This caught a real error: adding monomorphic map/filter/reduce as unconditional
    edges made the graph 100% dense. Those edges need a callback, and the fragment
    has no arrow functions, so they belong to the environment rather than the static
    graph. See the fragment+ho mode in type_graph.py.
    """
    u = universe(2)
    sizes = [bin(u.sources_reaching(g)).count("1") for g in range(len(u))]
    assert min(sizes) >= 1, "a type must at least reach itself"
    assert min(sizes) < len(u), "every type reaches some goal -- the filter is useless"
    pairs = sum(bin(r).count("1") for r in u.reach)
    density = 100.0 * pairs / (len(u) ** 2)
    print(f"      sources per goal: min {min(sizes)}, "
          f"median {sorted(sizes)[len(sizes)//2]}, max {max(sizes)} of {len(u)}; "
          f"{density:.0f}% dense")
    assert density < 95.0, f"{density:.0f}% dense leaves almost nothing filtered"


if __name__ == "__main__":
    import traceback

    failures = 0
    for name, fn in sorted(globals().items()):
        if not name.startswith("test_"):
            continue
        try:
            fn()
            print(f"pass  {name}")
        except Exception:
            failures += 1
            print(f"FAIL  {name}")
            traceback.print_exc()
    print(f"\n{failures} failure(s)")
    sys.exit(1 if failures else 0)
