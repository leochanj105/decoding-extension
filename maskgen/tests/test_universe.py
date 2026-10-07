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
    assert len(u) == 153, len(u)
    assert len(u.edges) == 934, len(u.edges)


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


def test_reachability_is_totally_connected():
    """Every type reaches every other, and that is the truth, not a bug.

    TypeScript converts anything to anything: numbers.map(someStr.charAt) is a
    string[], booleans.join(",").split(",") walks booleans to strings. Admitting a
    higher-order edge only when its callback type is independently obtainable still
    lands here, so the table filters nothing.

    Pinned as a test because it is the single most consequential fact about the
    design: a mask cannot get its selectivity from reachability. If a future change
    makes this fail, the filtering story changes with it.
    """
    u = universe(2)
    pairs = sum(bin(r).count("1") for r in u.reach)
    density = 100.0 * pairs / (len(u) ** 2)
    print(f"      {density:.0f}% dense -- reachability excludes nothing")
    assert density == 100.0, f"{density:.1f}% -- see FRAGMENT.md, this was 100%"


def test_restricted_graph_does_filter():
    """Without higher-order methods the table does filter, at 76%.

    Kept to show the contrast: the restriction is what creates filtering, and PLDI's
    own 45% comes from being restricted in a similar way.
    """
    u = TypeUniverse.for_fragment(max_depth=2, higher_order=False)
    pairs = sum(bin(r).count("1") for r in u.reach)
    density = 100.0 * pairs / (len(u) ** 2)
    print(f"      {len(u)} types, {density:.0f}% dense")
    assert len(u) == 125, len(u)
    assert 70.0 < density < 80.0, density


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
