# The TypeScript fragment we implement

Supersedes GRAMMAR.md, which described a three-type toy with a fixed prelude. That
toy was too easy: with no declarations during generation and a trivial type graph,
any approach wins.

The rule here is the opposite of the usual one for a subset. **Keep every feature
that creates type or environment difficulty; drop every feature that only creates
parsing.** TypeScript has far more syntax than type structure, and syntax is the
part XGrammar already handles.

## Types

Six concrete types, and nothing polymorphic:

```
number   string   boolean   number[]   string[]   boolean[]
```

No generics. `push` on `number[]` is simply `(number) => number`, so the member and
call edges survive without type parameters. This is what lets generics be dropped
without losing the graph.

**Member tables are ported from PLDI, not invented.** The measurement below is what
decided that: closing over their real member tables from these six types reaches
60% of the full environment's type graph, and a hand-picked member list would
reach far less.

## Grammar

```text
Program  ::= Stmt+

Stmt     ::= "let" Ident ":" Type "=" Expr ";"            -- introduces a typed name
           | Ident "=" Expr ";"                            -- goal type from the environment
           | "declare" "function" Ident "(" Ident ":" Type ")" ":" Type ";"

Expr     ::= Literal
           | Ident                                         -- variable reference
           | Expr "." Ident                                -- member access
           | Expr "." Ident "(" Args ")"                   -- method call
           | Ident "(" Args ")"                            -- call of a declared function
           | Expr "+" Expr
           | Expr "==" Expr
           | "(" Expr ")"

Args     ::= ε | Expr
Literal  ::= Number | String | Boolean | "[" ArrayItems "]"
Type     ::= "number" | "string" | "boolean"
           | "number[]" | "string[]" | "boolean[]"
Ident    ::= [a-zA-Z_] [a-zA-Z0-9_]*                       -- excluding reserved words
```

Semicolons are required; there is no automatic semicolon insertion. Whitespace is
`[ \t\n]` and at least one character must separate two adjacent word tokens.

## Why each feature is here

| feature | what it makes hard |
|---|---|
| `let` generated, not a prelude | names appear mid-generation -- the dynamic environment, which is the research question |
| member access `e.x` | the largest source of edges (50% of them) and of vocabulary churn: at `e.`&#8203;`|` the legal names are the members of `e`'s type, which is exactly a per-type mask |
| method and function calls | return-type edges, and a goal type that comes from a parameter |
| `declare function` -- signature, no body | puts function types in scope without function bodies, `return`, nested scopes or closures |
| assignment `x = e;` | a typed position whose goal comes from the environment rather than from an annotation |
| `+` and `==` | cross-type edges: `number + string` is a string, `==` yields boolean from anything |
| parentheses | keeps the precedence window real, so the context filter is non-trivial |
| arrays | `.length`, `.join`, `.indexOf`, `.concat` -- many edges, and nesting depth > 0 |

## What is excluded from PLDI, and what each exclusion costs

Everything PLDI supports that this fragment does not, with the measured effect and
what adding it back would unblock. This is the roadmap for reinstating features one
at a time.

### Types

| excluded | why it matters | adding it back unblocks |
|---|---|---|
| **`any` / `unknown`** | PLDI's wildcard: reachable from and to everything. 75% of its reachability queries have `any` as the goal and are vacuously true | `.filter`, `.every`, `.find`, `.forEach` -- all four are dropped solely because their signatures mention `any` |
| **type parameters (generics)** | `map`'s signature is `<T>(v0: string, ...) => T`; a `T` cannot be narrowed to one of the six | `.map`, and the whole generic-instantiation machinery PLDI handles with its `in_pattern` stack |
| **`void`** | appears as `forEach`'s return | `.forEach` (together with `any`) |
| **`RegExp`** | appears in `.match`, `.matchAll`, and in the unions of `.replace`, `.search`, `.split` | `.match`, `.matchAll`; the others are already kept by narrowing |
| **union types** | kept only by *narrowing* to their in-six branch, so `split(string \| RegExp)` becomes `split(string)` | true unions, and the branches we currently discard |
| **`null`, `undefined`, optional members** | optional chaining and the `T \| undefined` pattern PLDI pushes onto `in_pattern` | optional chaining |
| **objects, tuples, index signatures, BigInt, classes** | PLDI's full type lattice | user-defined types, and the other 68% of its type graph |

Only six types remain: `number`, `string`, `boolean` and arrays of them.

### Operators

PLDI has about thirty. The fragment keeps four: `.` `()` `+` `==`.

Excluded: indexing `[]`, arithmetic `-` `*` `/` `%` `**`, comparisons `<` `<=` `>`
`>=`, the other equalities `!=` `===` `!==`, bitwise `&` `^` `|` `<<` `>>` `>>>`,
logical `&&` `||`, ternary `?`, and `++`.

Measured consequence: **none, for reachability.** Tightening the precedence window
from "anything allowed" to "stronger than `+`" removes edges (715 to 480) without
changing what is reachable at all, because `.` and `()` sit at the top of the
precedence table, survive every window, and generate the graph by themselves. The
one exception is the receiver position of `.` or `(`, where no operator is allowed
and reachability collapses to the six base types.

That collapse is a property of having four operators. With PLDI's thirty it will not
hold -- their own trace shows precedence roughly tripling the number of distinct
reachability questions -- so this needs re-measuring as operators come back.

### Language features

| excluded | consequence |
|---|---|
| **lambdas / arrow functions** | function values can only come from a `declare function` or a method reference. In the empty environment only **11 of 36** one-argument function types over the six are constructible; with lambdas all 36 would be |
| **multi-argument declarations** | `declare function f(a: T): U` is unary, so a multi-argument callback cannot be declared. PLDI does accept a one-argument function where a three-argument callback is wanted, so this is less limiting than it looks |
| **function bodies, `return`** | no nested scopes and no closures, which is why the symbol table needs no block structure |
| control flow, classes, interfaces, enums, modules | no type-transition edges, only parsing |
| destructuring, spread, optional chaining, template literals | same |
| type inference | every declaration is annotated, so a required type is always known syntactically |
| comments, automatic semicolon insertion | parsing only; semicolons are required |

### Members

Of the 160 members on the six types, **127 (79%) stay inside the six**. Of the rest:

- dropped entirely: `.map` (mentions `T`), `.filter`, `.every`, `.find`,
  `.forEach` (mention `any`), `.match`, `.matchAll` (mention `RegExp`)
- kept by narrowing a union: `.split`, `.replace`, `.replaceAll`, `.search`
- kept as they are: `.reduce`, `.some`, `.sort`, `.join`, `.push`, and the rest

PLDI's 24 default global objects -- `Math`, `console`, `JSON` and so on -- are
excluded entirely. They and the types they pull in are most of the remaining gap.

### The net effect

| | PLDI | this fragment | kept |
|---|---:|---:|---:|
| types | 396 | 125 | 32% |
| edges | 2,371 | 750 | 32% |
| pairs reachable | 44.9% | 57.6% | -- |

Density is *higher* than PLDI's, which is not an error on either side: our graph
models generic instantiation nowhere, and PLDI's does not model it in reachability
either, but PLDI additionally gives up after 1000 search steps and reports "not
reachable". Both are approximations of different shapes.

## How much of the type graph this keeps



`maskgen/type_graph.py --max-depth 2`. Edges are counted by type equality, not by
string: a type and its optional-parameter spelling compare equal but stringify
differently, so counting strings miscounts.

| | types | edges | density | share of PLDI |
|---|---:|---:|---:|---|
| PLDI full environment | 396 | 2,371 | 44.9% | — |
| members within the six only | 101 | 621 | 40.9% | 26% / 26% |
| **the fragment** | **125** | **766** | **76.0%** | **32% / 32%** |
| the fragment + higher-order methods | 171 | 1,042 | 100% | 43% / 44% |

Counting the tables, 127 of 160 members (79%) already stay inside the six types.
The fragment recovers some of the other 21% by **narrowing unions to their in-six
branches**: `split(string | RegExp)` becomes `split(string)`, which is the common
case and the only thing that keeps `string -> string[]` reachable at all. The same
move recovers `replace`, `replaceAll` and `search`.

### Calling a function requires its arguments to be writable

This is the rule that makes the graph correct, and getting it wrong invalidated
three earlier versions of this section.

Member access always yields a method's type: `someStr.split` is a
`(string, number) => string[]` whether or not you call it. But *calling* it means
supplying every parameter, and in the fragment an argument can only be a literal, an
in-scope name, or another expression -- never an arrow function. So a call fires only
when every parameter type is obtainable, which is a fixpoint: each round makes more
types obtainable and so admits more calls. It settles in two rounds.

Without that gate every type reaches every other and the type filter is vacuous.
With it:

| | types | edges | pairs reachable |
|---|---:|---:|---:|
| PLDI full environment | 396 | 2,371 | 44.9% |
| **the fragment** | **125** | **750** | **57.6%** |
| the fragment with calls ungated | 125 | 766 | 76.0% |

And the concrete question that exposed the error:

```
number -> number[]    no    nothing turns a number into an array of numbers
number -> string[]    yes   (5 + "").split(",")
string -> string[]    yes   .split(",")
```

`number -> number[]` would need `map` or `reduce`. `.map` is gone for a different
reason -- its signature mentions a type parameter -- and `.reduce` survives but needs
a four-argument callback that nothing in the fragment produces, so the edge does not
exist. An ungated graph claimed it did.

Worth separating, because three different exclusions were at work and an earlier
version of this file credited all of it to the argument gate: `.map` is blocked by
generics, `.filter`/`.every`/`.find`/`.forEach` by `any`, and `.reduce` by the
argument gate. See the exclusions table above.

### Two checks, with opposite answers

A mask has to answer two questions at a typed position, and they are not the same.

**"Can this position be finished at all?"** -- always yes, but for a narrower reason
than first recorded here. Every type a program can *name* is constructible: all six
have literals. And a goal is always one of the six, since annotations draw from them
and so does `declare function f(a: T): U`. So no typed position is a dead end and
this check is vacuous.

The earlier claim that "all 125 types in the universe are constructible" was
circular: the universe is *defined* as what is reachable from the six, so everything
in it is reachable by construction. The real figures are all six annotatable types,
and **11 of the 36** one-argument function types over them. The other 25 -- including
`(boolean) => string` and `(number[]) => number` -- cannot be built at all, which is
precisely why the higher-order methods add no edges: `map` needs a callback, and most
callback types are unconstructible.

A corollary: there is no need for a symbol of each type to be in scope. Literals
cover every type, so a position is satisfiable with an empty environment. Symbol
availability decides only *which identifiers* appear in the mask.

**"Given a partial expression of type T, can G still be reached?"** -- 58% yes. From
a committed `number`, 60 of the 125 goals are unreachable. This is the real filter,
and it bites the moment anything is committed: at a `number[]` position the literal
`5` is already illegal, because `number` does not reach `number[]`.

### Where the filtering comes from

Reachability excludes about 42% of pairs, so it does real work -- but it is only part
of the story, and not the selective part. Three sources, in order:

1. **Exact type at a completion point.** At `let s: string = x|` the token `;` is
   illegal: ending here requires the expression to *be* a string, not merely to be
   convertible to one. Most type filtering lives here.
2. **The receiver's members after a dot.** At `someStr.|` only `string`'s members are
   legal -- a few dozen names rather than everything in scope. Highly selective, and
   it depends on the receiver's type, so it cannot be precompiled per grammar
   position.
3. **Reachability**, which rules out the 42% and keeps a prefix from being written
   when it is already doomed.

So the per-type masks in DESIGN.md want indexing by the exact required type at
completion points and by the receiver type at member positions, with reachability as
a cheaper pre-filter rather than the main mechanism.

## Caveats

- Three earlier versions of this section were wrong, all from the same cause:
  treating a method's return type as reachable without checking the method's
  arguments could be written. They claimed 60%/62%, then 43%/44%, then that the graph
  was 100% dense and reachability vacuous. The gate above is the fix, and a test
  pins `number` not reaching `number[]` so the error cannot recur silently.
- PLDI's own graph is not ground truth either. It omits generic instantiation,
  handling it lazily during the parse, and `_reachable_bfs` gives up after 1000
  iterations reporting "not reachable". Both make it stricter than the language.
- The graph is infinite without a depth bound, so only equal-depth comparisons
  mean anything.
- Nothing here is validated against generated code yet. Whether models can write
  interesting programs in this fragment is a separate question from whether the
  type reasoning is hard enough.
