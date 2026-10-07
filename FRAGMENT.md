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

`maskgen/type_graph.py --max-depth 2`, closing over PLDI's own edge functions:

| nesting depth | PLDI built-in environment | members within the six only | **the fragment** |
|---|---|---|---|
| <= 1 | 141 types, 810 edges | 54 / 339  (38% / 42%) | **77 / 477  (55% / 59%)** |
| <= 2 | 396 types, 2,344 edges | 101 / 621  (26% / 26%) | **147 / 897  (37% / 38%)** |

The middle column is what happens if every member whose signature mentions a type
outside the six is simply dropped. Counting the tables, 127 of 160 members (79%)
already stay inside, so only 21% are affected -- but they carry more than half the
graph, because the array methods they contain take callbacks.

The right column adds those methods back by **instantiating them at concrete
types**: `number[].map` with a `(number) => string` callback yields `string[]`.
Nine instantiations per array type replace one generic signature. This is the same
trick that turns `push` into `(number) => number`, applied to `map`, `filter`,
`reduce`, `some` and `every`, and it recovers the graph from 26% to 37%.

So the fragment keeps roughly **37% of the types and 38% of the edges** of PLDI's
full environment at nesting depth 2, in absolute terms 147 types and 897 edges.
Edges by kind at depth 2 for the full environment: member access 1,167, `+` 398,
`==` 396, calls 362, indexing 11, logical and ternary 18 each.

The remaining gap is PLDI's 24 default global objects (`Math`, `console`, `JSON`
and so on) and the types they drag in. Closing it is pure porting of more member
tables and needs no new mechanism, so it is a lever to pull if 37% proves too thin
rather than a design problem.

Two things this is *not* explained by:

- **Not the `any` wildcard.** Only 6% of PLDI's edges at depth 2 touch `any` or
  `unknown`; 2,207 of 2,344 run between concrete types. An earlier guess that their
  count was inflated by a vacuous wildcard was wrong.
- **Not nesting depth.** The three columns are compared at equal depth throughout.

## Caveats

- The counts close over PLDI's member tables from our six seed types, so they
  describe the fragment *as specified*. An earlier version of this file claimed
  60% / 62%; that measurement kept every member, including the generic `map`,
  `filter` and `reduce` that the fragment excludes, so it was not self-consistent.
  The figures above are.
- The graph is infinite without a depth bound, so only equal-depth comparisons
  mean anything.
- Nothing here is validated against generated code yet. Whether models can write
  interesting programs in this fragment is a separate question from whether the
  type reasoning is hard enough.
