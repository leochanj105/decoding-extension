# Where the fragment stands, and the gap to PLDI

What is built and measured today, and what separates it from the TypeScript fragment
in the PLDI'25 type-constrained decoding paper. Two companions: `FRAGMENT.md` has the
feature-by-feature exclusion analysis with the measurements behind each decision, and
`PIPELINE.md` has the implementation and the audit of XGrammar's internals.

Tagged `fragment-v1` in this repository and in the `xgrammar` submodule.

---

## Part 1 — what exists

### The language

Six types, and nothing polymorphic:

```
number   string   boolean   number[]   string[]   boolean[]
```

Three statement forms:

```
let x : string = e ;                                  an annotation sets the goal type
x = e ;                                               the target's declared type does
declare function f ( a : number ) : string ;          a signature, no body
```

Expressions: names drawn from the symbol table, member access, calls with and
without an argument, chains of both (`msg.split(msg).length`), number / string /
boolean / array literals including `[]`, `+` typed from both operands, `==`, and
parentheses.
Whitespace is free everywhere except inside a postfix chain, where it is not allowed.

**A member access hands back the member, and a method is a function.** `msg.split` is
"takes a string, gives a string[]"; only `msg.split(msg)` is a string[]. A property is
not a function: `msg.length` is a number on its own. Modelling it the other way round
-- which this fragment did until calls were implemented -- accepts TypeScript that
does not type check, and leaves the call machinery doing no work at all.

Three files, with one job each:

| file | job |
|---|---|
| `maskgen/fragment.ebnf` | what text is well formed. 27 rules. **No type is named in it** |
| `maskgen/fragment.types` | what each rule does to the types. 16 rows, one per rule |
| `maskgen/cpp/type_table*.h` | reads the table, applies it, and refuses to run if the two files disagree |

Declared variables reach the symbol table as the program is written, so a later
statement can use a name an earlier one declared. A name is not in scope inside its
own declaration, since nothing is registered until the statement's `;` lands. A
declaration may not reuse a name already in scope, nor a reserved word -- checked
when the name ends, so `counter` is fine even though `count` is taken. This
rides on XGrammar's captures, which are recorded only on committed tokens and rolled
back with the parser -- a hook on the rules would fire thousands of times during mask
generation, on paths the model never takes. `declare function` does not register
anything yet.

Two values travel with the parser: what this position must end up producing, and
what has been produced here so far. Each row of the table says how one rule
transforms them — inherit, start fresh, drop the requirement, require a fixed type;
and on finishing: pass, keep the first, replace, produce a fixed type, produce or
require whatever the text names, check, or combine two types under an operator.

### What is verified

99 checks across seven test files, all passing.

- **51 checks on the type table alone**, with no parser, grammar or token involved.
  Two integers and a rule name in, two integers out.
- **48 checks end to end**, driving the real grammar with real vocabulary tokens the
  way a decoder does. A seven-statement program is accepted token by token, 63 of 63.
  Twelve ill-typed programs are each refused, at a named position. Sixteen well-typed
  ones are accepted.

Worth singling out, because each was a bug first:

```
let a : number   = msg.split.length ;   accepted   a chain: string -> string[] -> number
let a : string   = msg + 3 ;            accepted   + typed from both operands
let a : number   = 3 + msg ;            refused    ...which makes this one wrong
let a : boolean  = count == total ;     accepted   a comparison is a boolean
let a : number   = count == total ;     refused    ...not its operands' type
let a : number   = ( count ) ;          accepted   a checked expression keeps its type
let a : boolean[] = msg ;               refused    at the FIRST NAME, not the semicolon
let a : string   = msg.toUpperCase() ;  accepted   a method is a function; () applies it
let a : string   = msg.toUpperCase ;    refused    the function itself is not a string
let a : string[] = msg.split() ;        refused    split takes a string, not nothing
let a : number   = msg.length() ;       refused    length is a property, not a method
let a : number   = count() ;            refused    a number is not callable
let a : number   = msg.split(msg).length ;  accepted   string -> function -> string[] -> number
```

That last one is the shape of the whole thing: nothing in the environment can produce
a `boolean[]`, so the model is not allowed to start down a road with no end.

### What it costs

`maskgen/cpp/bench_e2e.cc`. Every position of the program measured once, in order,
the whole walk repeated with a fresh matcher. Never one position in a loop, which
answers a question no decoder asks.

```
                                    mean     median      worst      total
mask generation                   160 us      96 us     979 us    15.0 ms
the same language, none of this   222 us      17 us    1339 us    20.9 ms
accept and advance one token      2.2 us     1.5 us     9.5 us    0.21 ms
compiling the grammar                                             121 ms (once)
```

Nine statements, 94 tokens, including calls, an array literal, and two statements
that use names earlier ones declared.

A model step is about 33,000 µs, so the mean mask is **0.48% of a step** and the worst
position 3.0%. Against the same language with none of this machinery, the type
checking makes mask generation **faster** — 10.3 ms against 16.6 ms — because cutting
76,000 candidates to a few hundred costs less than walking them. There is no overhead
to defend.

---

## Part 2 — the gap to PLDI

Measured over the closed type graph, `maskgen/type_graph.py --max-depth 2`:

| | PLDI | this fragment | kept |
|---|---:|---:|---:|
| types | 396 | 125 | 32% |
| type-transition edges | 2,371 | 750 | 32% |
| share of pairs reachable | 44.9% | 57.6% | — |

Density is *higher* here, which is not an error on either side: both are
approximations of different shapes. Ours models generic instantiation nowhere;
PLDI's does not model it in reachability either, but additionally gives up after
1,000 search steps and answers "not reachable", so it is stricter than the language
in a different way.

**One figure to keep in mind before reading 32% as "a third done":** 75% of PLDI's
reachability queries have `any` as the goal, and are therefore vacuously true. A
large share of the work we are not doing is work that answers itself.

### Still missing from our own fragment

Small, and listed first because they are cheapest:

| | |
|---|---|
| **member access on an expression** | only a *name* takes `.member` or `(args)`. `[1, 2].length`, `"abc".length` and `( e ).member` are not writable, where PLDI allows a member access on any expression. |
| **multi-argument functions** | ours is unary. PLDI accepts a one-argument function where a multi-argument callback is wanted, so this is less limiting than it sounds. |

| | |
|---|---|
| **member access on an expression** | only a *name* takes `.member` or `(args)`. `[1, 2].length`, `"abc".length` and `( e ).member` are not writable, where PLDI allows a member access on any expression. |


### Gap that is only more data

Our machinery handles each of these as it stands. Someone has to port tables.

| missing | effect |
|---|---|
| the 24 default global objects (`Math`, `console`, `JSON`, …) | the largest single remaining item: they and the types they pull in are most of the gap |
| 26 of ~30 operators: `[]` `-` `*` `/` `%` `**` `<` `<=` `>` `>=` `!=` `===` `!==` bitwise, `&&` `\|\|` `?` `++` | **no effect on reachability, measured.** `.` and `()` sit at the top of the precedence table and generate the graph by themselves. But PLDI's trace shows precedence roughly tripling the number of distinct reachability questions, so they make the *context filtering* harder without making the graph bigger. Needs re-measuring as they come back. |
| multi-argument `declare function` | ours is unary. PLDI accepts a one-argument function where a three-argument callback is wanted, so this is less limiting than it looks |
| the higher-order methods `.map` `.filter` `.every` `.find` `.forEach` | would take the graph from 32% to **43%** — but they are blocked by generics, `any` and `void`, so they are not pure data. See below. |

### Gap that needs a different design

These do not extend the approach; they invalidate an assumption it rests on.

**The assumption.** The list of every type that can ever arise is finite and known
before generation starts — 125 entries, written down once. That makes one table
possible, covering all 125 × 125 pairs of "can an A still become a B?", and turns
every question asked during generation into a single bit lookup. Every fast number in
Part 1 comes from that table existing.

| missing | what it breaks |
|---|---|
| **generics**, and **object types, tuples, classes, index signatures** | The model can invent types as it writes: `Array<Array<string>>` nests without bound, and `{ name: string }` is a type nobody listed. The list stops being finite, so there is no table, so there is no bit lookup. This is the single exclusion that invalidates the design rather than shrinking it — and it is exactly why PLDI searches per question and needs a 1,000-step cap where we need neither. |
| **lambdas / arrow functions** | Only **11 of the 36** one-argument function types over the six are constructible today; with lambdas, all 36. This is also why the higher-order methods currently add nothing: `map` wants a callback whose type nothing in the fragment can build. |
| **function bodies and `return`** | Nested scopes and closures. Our symbol table has no block structure because nothing has needed one. |
| **`any` / `unknown`** | Reachable from and to everything. The closure still works; the filter just stops doing much. See the 75% figure above. |
| **true union types, `null` / `undefined`, optional members** | Unions survive here only by narrowing to their in-six branch — `split(string \| RegExp)` becomes `split(string)`, which is what keeps `string -> string[]` reachable at all. Real unions and optional chaining need the branch machinery PLDI carries on a stack. |

### Excluded with no consequence for types

Control flow, classes as syntax, interfaces, enums, modules, destructuring, spread,
template literals, type inference, comments, automatic semicolon insertion. These
create parsing work, which is the part XGrammar already does, and no type-transition
edges. The fragment drops them on purpose.

---

## Part 3 — the honest summary

What can be claimed today: **the full mechanism, running at 0.5% of a model step,
over a type universe of 125 types closed in advance.** Type-directed name filtering,
member access typed by its receiver, operators typed from both operands, early
refusal of positions that cannot be completed, and a mask that never refuses a legal
token.

What cannot: that it covers PLDI's language. It covers about a third of PLDI's type
graph, and the third it covers is the part where reachability is a real question
rather than a vacuous one.

The globals and the operators are volume — port them and nothing about the approach
changes. Generics and object types are the frontier, because supporting them means
giving up the closed universe, and with it the precomputed table that makes all of
this fast. That is likely a hybrid: the table for the closed part, a bounded search
for the rest, which is PLDI's answer and ours together.
