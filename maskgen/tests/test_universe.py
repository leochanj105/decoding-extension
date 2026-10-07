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
    assert len(u.edges) == 750, len(u.edges)


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


def test_a_number_does_not_become_an_array_of_numbers():
    """The test that caught three wrong conclusions in a row.

    Nothing in the fragment turns a number into a number[]. Reaching one needs
    `map` or `reduce` with a callback, and with no arrow functions the only function
    values available are declared functions and method references, none of which has
    the required shape.

    A number *does* reach string[], via (5 + "").split(","), which needs no callback.

    Leaving calls ungated -- treating a method's type as reachable without checking
    its arguments can be written -- makes every type reach every other and the whole
    type filter vacuous. That is what went wrong three times.
    """
    u = universe(2)
    num = u.id_of(NumberPType())
    numbers = u.id_of(ArrayPType(NumberPType()))
    strings = u.id_of(ArrayPType(StringPType()))
    assert not u.reaches(num, numbers), "number should not reach number[]"
    assert u.reaches(num, strings), 'number -> string[] via (5 + "").split(",")'


def test_every_type_is_constructible_from_literals():
    """No typed position is ever a dead end, so "can this be finished?" is always yes.

    Every one of the six base types has a literal, and everything else in the universe
    is reachable from one, so any required type can be produced from nothing. That
    makes the "is there any completion here?" check vacuous and deletable.

    What is *not* vacuous is the other check: once a partial expression has committed
    to a type T, only 58% of goals remain reachable. At a number[] position the
    literal 5 is already illegal, because number does not reach number[].
    """
    u = universe(2)
    from maskgen.type_graph import six_types

    constructible = 0
    for seed in six_types():
        constructible |= u.reach[u.id_of(seed)]
    built = bin(constructible).count("1")
    print(f"      {built}/{len(u)} types constructible from literals alone")
    assert built == len(u), f"only {built} of {len(u)} types can be written"


def test_committing_to_a_type_does_exclude_goals():
    """The filter bites once a type is committed, not before."""
    u = universe(2)
    num = u.id_of(NumberPType())
    reachable = bin(u.reach[num]).count("1")
    print(f"      from a committed number, {reachable}/{len(u)} goals remain")
    assert reachable < len(u), "a committed number should not reach every goal"
    assert not u.reaches(num, u.id_of(ArrayPType(NumberPType())))


def test_reachability_filters_a_real_share():
    """Reachability must exclude a substantial fraction, or it is doing no work."""
    u = universe(2)
    pairs = sum(bin(r).count("1") for r in u.reach)
    density = 100.0 * pairs / (len(u) ** 2)
    print(f"      {density:.0f}% of pairs reachable "
          f"(PLDI's own environment: 45%)")
    assert 40.0 < density < 75.0, (
        f"{density:.0f}% -- near 100 means calls are ungated, near 0 means the "
        "member tables are not being read"
    )


def test_ungated_calls_destroy_the_filter():
    """Pins the failure mode, so a regression is a test failure and not a surprise."""
    u = TypeUniverse.for_fragment(max_depth=2, gated=False)
    pairs = sum(bin(r).count("1") for r in u.reach)
    density = 100.0 * pairs / (len(u) ** 2)
    print(f"      ungated: {len(u)} types, {density:.0f}% dense")
    assert density > 70.0, "expected the ungated graph to be much denser"


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
