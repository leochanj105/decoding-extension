"""Does the parser report the right scope and the right required type?

These are the two things a mask cannot get from PLDI, so they are the two things
worth pinning hard.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))

from maskgen.program import (  # noqa: E402
    WANT_ASSIGN,
    WANT_EXPRESSION,
    WANT_NEW_NAME,
    WANT_STATEMENT,
    WANT_TYPE,
    ParseError,
    analyse,
)
from typesafe_llm.parser.types_ts import (  # noqa: E402
    ArrayPType,
    BooleanPType,
    FunctionPType,
    NumberPType,
    StringPType,
)


def test_walks_a_declaration():
    steps = [
        ("", WANT_STATEMENT),
        ("let ", WANT_NEW_NAME),
        ("let x : ", WANT_TYPE),
        ("let x : number = ", WANT_EXPRESSION),
        ("let x : number = 1;", WANT_STATEMENT),
    ]
    for text, want in steps:
        assert analyse(text).want == want, (text, analyse(text).want)


def test_declaration_takes_effect_only_after_the_semicolon():
    """`let s : string = s;` must not see s, which is what PLDI does too."""
    inside = analyse("let s : string = ")
    assert "s" not in inside.scope, "s is visible inside its own initializer"
    after = analyse("let s : string = \"hi\";")
    assert after.scope["s"] == StringPType()


def test_required_type_comes_from_the_annotation():
    assert analyse("let x : number = ").required_type == NumberPType()
    assert analyse("let s : string = ").required_type == StringPType()
    assert analyse("let b : boolean = ").required_type == BooleanPType()


def test_array_annotations():
    p = analyse("let xs : number[] = ")
    assert p.required_type == ArrayPType(NumberPType())
    p = analyse('let xs : string[] = ["a"]; let n : number = ')
    assert p.scope["xs"] == ArrayPType(StringPType())


def test_required_type_of_an_assignment_comes_from_the_target():
    p = analyse("let s : string = \"a\"; s = ")
    assert p.want == WANT_EXPRESSION
    assert p.required_type == StringPType()


def test_declared_function_enters_scope_with_a_function_type():
    p = analyse("declare function f(a: number): string; ")
    assert p.scope["f"] == FunctionPType(
        call_signature=(NumberPType(),), return_type=StringPType()
    )
    # the parameter name is not in scope: there is no body
    assert "a" not in p.scope


def test_required_type_for_a_call_target_is_the_function_type():
    p = analyse("declare function f(a: number): string; let s : string = ")
    assert p.required_type == StringPType()
    assert sorted(p.scope) == ["f"]


def test_pending_lexeme_is_reported():
    p = analyse("let co")
    assert p.pending is not None and p.pending.text == "co"
    p = analyse("let co ")
    assert p.pending is None


def test_brackets_inside_an_expression_do_not_end_the_statement():
    p = analyse('let s : string = f(g(";"))')
    assert p.want == WANT_EXPRESSION, p
    p = analyse('let s : string = f(g(";"));')
    assert p.want == WANT_STATEMENT and "s" in p.scope


def test_scope_accumulates():
    p = analyse('let x : number = 1; let s : string = "a"; let b : boolean = true; ')
    assert sorted(p.scope) == ["b", "s", "x"]


def test_errors():
    bad = {
        "q = ": "assigning to an undeclared name",
        "let x : number = 1; let x : ": "declaring x twice",
        "let 9x : ": "a name starting with a digit",
        "= 1;": "a statement starting with =",
        "let x : number = ;": "an empty expression",
        "let x : number = )": "an unmatched bracket",
        "declare fun ": "the wrong keyword after declare",
    }
    for text, why in bad.items():
        try:
            got = analyse(text)
        except ParseError:
            continue
        raise AssertionError(f"{why}: {text!r} should not parse, got {got}")


def test_an_unfinishable_partial_word_is_rejected():
    """`let x : num` can still become `number`; `let x : widget` can become nothing."""
    analyse("let x : num")                       # fine
    analyse("let x : number")                    # fine
    for bad in ("let x : widget", "let x : q", "declare fx"):
        try:
            analyse(bad)
        except ParseError:
            continue
        raise AssertionError(f"{bad!r} has no legal continuation and should be refused")


def test_a_partial_name_must_match_something_in_scope():
    """At a statement start a name must already exist, so the prefix must match one."""
    analyse("let count : number = 1; cou")       # could still become `count`
    analyse("let count : number = 1; l")         # could still become `let`
    analyse("let count : number = 1; de")        # could still become `declare`
    try:
        analyse("let count : number = 1; q")
    except ParseError:
        return
    raise AssertionError("no name or keyword starts with q, so it should be refused")


def test_expected_set_is_available_for_the_mask():
    """The mask is built from this, so it has to be exposed and correct."""
    assert analyse("let x : ").expected().words == frozenset(
        {"number", "string", "boolean"}
    )
    assert analyse("let ").expected().fresh_name is True
    at_stmt = analyse("let x : number = 1; ").expected()
    assert at_stmt.words == frozenset({"let", "declare"})
    assert at_stmt.names == frozenset({"x"})
    # note the trailing space: without it `number` is still the pending lexeme and
    # has not been consumed, so we are still expecting the type word
    assert analyse("let x : number ").expected().punctuation == frozenset({"[", "="})
    assert analyse("let x : number").want == WANT_TYPE


def test_inside_an_expression_nothing_is_checked_yet():
    """Honest gap: the expression body is opaque, so anything is admitted there.

    `let x : number = zzz` is accepted although zzz is not in scope. Closing this is
    the next stage -- expression parsing with the committed type tracked -- not a
    design boundary.
    """
    position = analyse("let x : number = zzz")
    assert position.want == WANT_EXPRESSION
    assert position.expected().anything is True


def test_valid_prefixes_do_not_raise():
    for text in ["", "l", "let", "let ", "let x", "let x ", "let x :", "let x : n",
                 "let x : number", "let x : number ", "let x : number =",
                 "let x : number = ", "let x : number = 1", "let x : number = 1;",
                 "declare", "declare function f(a", "declare function f(a: number",
                 "declare function f(a: number)", "declare function f(a: number):"]:
        analyse(text)


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
