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

## Deliberately excluded

Generics, control flow, classes, interfaces, enums, modules and imports, arrow
functions, function bodies, `return`, destructuring, spread, optional chaining,
template literals, union types, `null` and `undefined`, optional members, type
inference (every declaration is annotated), and comments.

None of these add type-transition edges. All of them add parsing.

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

### Higher-order methods are environment-dependent, not static

`map`, `filter`, `reduce`, `some` and `every` are excluded from the static graph,
and the last row shows why: adding them makes the graph **100% dense**, so every
type reaches every goal and the type filter selects nothing.

That is not a measurement artefact, it is the fragment's own rule. `number[].map`
reaches `string[]` only by supplying a `(number) => string` callback, and the
fragment has no arrow functions -- the only way to obtain a function value is to
name a `declare function`. So the edge exists only once such a function has been
declared. It belongs to the environment, not to the static universe.

They are kept in a separate mode (`fragment+ho`) to be added back deliberately,
at which point type reachability stops being static and has to be maintained
incrementally.

### What the density means

At 76% the static filter rejects only about a quarter of (source, goal) pairs, so
it is weak at the *start* of an expression. That is correct and matches PLDI: at
`let s: string = |` it allows `x` even when `x` is a number, because `x.toString()`
is a string. The constraint bites when the expression must **end** -- `x;` is
rejected -- and a completion position demands an exact type, not reachability.

So the value of the design is not a strong reachability filter. It is doing the
whole job, syntax and types, at both expression start and completion, as a mask
lookup rather than a per-candidate parse.

The remaining gap to PLDI is their 24 default global objects (`Math`, `console`,
`JSON`) and the types they pull in. Closing it is pure porting of more member
tables with no new mechanism.

Not explained by the `any` wildcard: only 6% of PLDI's edges touch `any` or
`unknown`, so their count is not inflated by a vacuous type.

## Caveats

- Two earlier versions of this file were wrong. The first claimed 60% / 62%,
  measured while keeping every member including the generics the fragment excludes.
  The second claimed 43% / 44%, measured with those generics monomorphised as
  unconditional edges, which silently made the graph 100% dense. The figures above
  separate the static graph from the environment-dependent part.
- PLDI's own graph is not ground truth either: it omits generic instantiation
  entirely, handling it lazily during the parse, so its 44.9% density
  under-approximates what is actually reachable.
- The graph is infinite without a depth bound, so only equal-depth comparisons
  mean anything.
- Nothing here is validated against generated code yet. Whether models can write
  interesting programs in this fragment is a separate question from whether the
  type reasoning is hard enough.
