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

`maskgen/type_graph.py`, closing over PLDI's own edge functions:

| nesting depth | PLDI built-in environment | this fragment | kept |
|---|---|---|---|
| <= 1 | 141 types, 810 edges | 102, 626 | 72% / 77% |
| <= 2 | 396 types, 2,344 edges | 239, 1,453 | **60% / 62%** |

Average out-degree is 6.1 against 5.9, so the shape of the graph is preserved and
only its size differs. The missing 40% is PLDI's 24 default global objects
(`Math`, `console`, `JSON` and so on) and the types they drag in; adding one or two
of them later would close most of the gap without new machinery.

Edges by kind at depth 2: member access 1,167, `+` 398, `==` 396, calls 362,
indexing 11, logical and ternary 18 each.

## Caveats

- The counts close over PLDI's member tables from our six seed types, so they
  describe the fragment *as specified* -- with their tables ported. Trimming the
  tables trims the graph proportionally.
- The graph is infinite without a depth bound, so only equal-depth comparisons
  mean anything.
- Nothing here is validated against generated code yet. Whether models can write
  interesting programs in this fragment is a separate question from whether the
  type reasoning is hard enough.
