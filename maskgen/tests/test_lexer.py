"""Does the lexer get the complete/incomplete boundary right?

That boundary is the whole point: a mask mid-identifier must allow the identifier to
grow, and a mask after a finished identifier must not.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))

from maskgen.lexer import KIND_PUNCT, KIND_STRING, LexError, lex, split_settled  # noqa


def kinds(text):
    return [(l.kind, l.text, l.complete) for l in lex(text)]


def test_simple_statement():
    assert kinds("let x : number = 1;") == [
        ("word", "let", True), ("word", "x", True), ("punct", ":", True),
        ("word", "number", True), ("punct", "=", True),
        ("number", "1", True), ("punct", ";", True),
    ]


def test_trailing_word_is_incomplete():
    """After `let ms` the word may still grow into `msg`."""
    got = lex("let ms")
    assert got[-1].text == "ms" and not got[-1].complete


def test_word_followed_by_space_is_complete():
    got = lex("let ms ")
    assert got[-1].text == "ms" and got[-1].complete


def test_lone_equals_at_the_end_may_still_become_double():
    got = lex("x =")
    assert got[-1].text == "=" and not got[-1].complete
    got = lex("x = ")
    assert got[-1].complete
    got = lex("x ==")
    assert got[-1].text == "==" and got[-1].complete


def test_unterminated_string_is_incomplete():
    got = lex('s = "hel')
    assert got[-1].kind == KIND_STRING and not got[-1].complete
    got = lex('s = "hel"')
    assert got[-1].complete and got[-1].text == '"hel"'


def test_reserved_words_are_not_identifiers():
    for word in ("let", "declare", "function", "number", "true"):
        assert lex(word + " ")[0].is_reserved
        assert not lex(word + " ")[0].is_identifier
    assert lex("count ")[0].is_identifier


def test_double_equals_wins_over_single():
    got = lex("a == b ")
    assert (got[1].kind, got[1].text) == (KIND_PUNCT, "==")


def test_illegal_character_is_rejected():
    for bad in ("x @ y", "a # b", "v $ w"):
        try:
            lex(bad)
        except LexError:
            continue
        raise AssertionError(f"{bad!r} should not lex")


def test_split_settled():
    settled, pending = split_settled(lex("let ms"))
    assert [l.text for l in settled] == ["let"]
    assert pending.text == "ms"
    settled, pending = split_settled(lex("let ms "))
    assert [l.text for l in settled] == ["let", "ms"]
    assert pending is None


def test_offsets_point_into_the_source():
    text = "let count : number = 42;"
    for lexeme in lex(text):
        assert text.startswith(lexeme.text, lexeme.start), lexeme


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
