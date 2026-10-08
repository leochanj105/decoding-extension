# The fragment's grammar, and how the type requirement flows through it

Two separate things, kept separate on purpose:

- **the grammar** — plain EBNF, exactly what XGrammar is given, with no type information in it
- **the requirement flow** — a side table saying, at each position, what type is needed

XGrammar parses the first and knows nothing of the second. That is what lets the
grammar stay small instead of being generated with one rule per type.

## The grammar

```ebnf
program      ::= stmt+

stmt         ::= let_stmt | assign_stmt | declare_stmt
let_stmt     ::= "let" sp ident ws ":" ws type ws "=" ws expr ws ";"
assign_stmt  ::= ident ws "=" ws expr ws ";"
declare_stmt ::= "declare" sp "function" sp ident ws "(" ws ident ws ":" ws type
                 ws ")" ws ":" ws type ws ";"

type         ::= base ("[" "]")?
base         ::= "number" | "string" | "boolean"

expr         ::= eq
eq           ::= sum (ws "==" ws sum)?
sum          ::= primary (ws "+" ws primary)*
primary      ::= num_lit | str_lit | bool_lit | arr_lit
               | postfix
               | "(" ws expr ws ")"

postfix      ::= __lex_name trailer*
trailer      ::= "." __lex_member | "(" ws expr ws ")"

num_lit      ::= [0-9]+
str_lit      ::= "\"" [^"\n]* "\""
bool_lit     ::= "true" | "false"
arr_lit      ::= "[" ws (expr (ws "," ws expr)*)? ws "]"

ident        ::= [a-zA-Z_] [a-zA-Z0-9_]*
__lex_name   ::= [a-zA-Z_] [a-zA-Z0-9_]*
__lex_member ::= [a-zA-Z_] [a-zA-Z0-9_]*

sp           ::= [ \t\n]+
ws           ::= [ \t\n]*
```

`eq` and `sum` exist only to give `==` and `+` their precedence, so that
`a + b == c` groups as `(a + b) == c` and `b.c` binds tighter than `a + b`. That
is the part of precedence XGrammar handles unaided, and the reason the reachability
side of precedence can be skipped — see PLAN.md.

**Only two rules are lexicon rules**, `__lex_name` and `__lex_member`. They are not
generated per type: which names they offer comes from the requirement carried in the
parser state, not from the rule's identity. `ident` is a plain rule because a name
being *declared* is unconstrained.

## The requirement

A position carries a **residual**: a pair.

```
(have, need)
```

`have` is the type of what has been written in this sub-expression so far, `⊥`
before anything. `need` is the type the position must end up producing.

Two checks, which is the whole type system:

| check | when | rule |
|---|---|---|
| still alive | every token | `have` can still reach `need`, or `have = ⊥` |
| may finish | wherever the expression could end | `need` accepts `have` exactly |

The second is why `let s : string = count` can be written but `let s : string = count;`
cannot: `count` is a `number`, which reaches `string`, so the name is admitted, but
ending there requires `have = string`.

## How the residual flows

| position | residual there |
|---|---|
| `let x : T = ▮` | `(⊥, T)` — from the annotation just parsed |
| `x = ▮` where `x : T` | `(⊥, T)` — from the target's declared type |
| `f(▮)` where `f : (P) => R` | `(⊥, P)` — from the parameter |
| `▮ + e` and `e + ▮` | `(⊥, unconstrained)`; the sum's own type is then checked against `need` |
| `▮ == e` | `(⊥, unconstrained)`; `==` produces `boolean`, so `==` is legal only where `boolean` reaches `need` |
| `[ ▮ ]` with `need = E[]` | `(⊥, E)` |
| `( ▮ )` | the enclosing residual, unchanged |
| after `__lex_name` matched name `n : T` | `have` becomes `T` |
| after `. m` where `m : M` | `have` becomes `M` |
| after a call on `have = (P) => R` | `have` becomes `R` |

Only three rows set a requirement from text the model wrote — the `let`
annotation, the assignment target, and a call's parameter. Everything else either
inherits or is fixed.

## What each lexicon rule offers

At `__lex_name` with residual `(⊥, need)`: every in-scope name whose type can reach
`need`. Not only names of type `need` — `count` is a `number` and `count.toString()`
is a `string`, so it is admitted at a string position.

At `__lex_member` with residual `(have, need)`: every member of `have` whose own type
can reach `need`. Both halves are static, so the whole table is precomputed — 127
members over the six receiver types collapse to **7 distinct name sets** across the
36 (receiver, required) pairs, and 10 of those pairs admit no member at all.

Literals are the same mechanism with a static name list: `num_lit` is admitted
exactly where `number` reaches `need`, and so on. That is PLDI's structure — every
form declares its type and one reachability check filters all of them.

## Ambiguity: many residuals at once

The parser holds many readings simultaneously, and they can disagree about the
residual. Three kinds occur.

**Syntactic ambiguity, same residual.** After `count` the text may be finished, or
continue as `count.length`, or as `count(…)`. Three live readings, all with the same
`(number, need)`. Earley already keeps one state per alternative and the mask is
their union; nothing new is needed.

**A prefix matching names of different types.** With `cat : string` and `count :
number` both in scope, after `c` both are still possible, so `have` is not yet
determined. This is not represented as two states: `have` only becomes definite when
the name is *complete*, and a completed name is a single string with a single type.
What narrows during typing is the candidate set inside the lexicon, not the state.

The consequence is that the lexicon has to gate completion: `__lex_name` matches any
identifier syntactically, so the reading where the name ends at `cou` must be killed,
because `cou` is not declared. That is what `IsCompleteName` is for, and it is the
part not yet wired in.

**Genuinely different residuals.** `f(g(▮))` with `f : (number) => string` and
`g` overloaded, or an outer `+` whose operand type is still open. Here readings do
differ in `have` or `need`, and they must not be merged — different tokens are legal
in each. This is why the residual belongs in the parser state rather than beside it:
a side stack cannot say which reading a residual belongs to.

The cost is that two readings at one grammar position with different residuals no
longer merge, so more states stay alive. The compensation is that the mask for
`(position, residual)` is cacheable, and on one real program PLDI's 86,106 type
queries collapsed to 253 distinct keys — a few hundred cache entries for a whole
program.
