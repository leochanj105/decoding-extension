"""Does the oracle report what PLDI actually believes?

These tests pin down the legality semantics, because getting them wrong would
make every later comparison meaningless.
"""

import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))

from maskgen.oracle import Oracle  # noqa: E402

PRELUDE = "let x: number = 1;\nlet s: string = "


def test_start_is_alive():
    assert Oracle.at_start().alive


def test_advance_is_pure():
    base = Oracle.at_start().advance(PRELUDE)
    before = base.readings
    assert before > 0
    base.advance('"hi";')
    base.advance("nonsense !!")
    assert base.readings == before, "trialling a candidate disturbed the base state"


def test_legality_is_about_continuation_not_immediate_type():
    at = Oracle.at_start().advance(PRELUDE)       # a string is required here
    # a number-typed name is fine as a prefix: x.toString() and x + "a" are strings
    assert at.allows("x")
    # but committing it as a bare number is not
    assert not at.allows("x;")
    # a string literal is fine either way
    assert at.allows('"')
    assert at.allows('"hi";')


def test_name_being_declared_is_not_yet_in_scope():
    at = Oracle.at_start().advance(PRELUDE)
    assert not at.allows("s"), "s should not be visible inside its own initializer"


def test_dead_state_rejects_everything():
    dead = Oracle.at_start().advance("let 9bad")
    assert not dead.alive
    assert not dead.allows("x")


def test_allowed_among_filters():
    at = Oracle.at_start().advance(PRELUDE)
    got = at.allowed_among([(1, '"'), (2, "x;"), (3, "x"), (4, "s")])
    assert got == {1, 3}, got


def test_query_cost_is_known():
    """Records the per-query cost, which sets the sampling budget in check.py."""
    at = Oracle.at_start().advance(PRELUDE)
    cands = ['"', "x", "1", "true", "x;", "s", "f", "to", "Str", "(", ")", " "]
    start = time.perf_counter()
    for _ in range(5):
        for c in cands:
            at.allows(c)
    per = (time.perf_counter() - start) / (5 * len(cands))
    print(f"      oracle: {per*1000:.2f} ms/query -> "
          f"{151665*per:.0f}s to sweep the whole vocabulary")
    assert per < 0.5, "unexpectedly slow; the sampling budget assumes well under this"


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
