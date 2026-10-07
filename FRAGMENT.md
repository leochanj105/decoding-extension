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

### Reachability does not filter, and that is a fact about TypeScript

`map`, `filter` and `reduce` make the graph **100% dense**: every type reaches every
other type, so asking "can this type still become the goal type?" always answers
yes.

The first instinct was that this was a modelling error, because `map` needs a
callback and the fragment has no arrow functions. It is not. A callback can be a
*method reference*: `someStr.charAt` is already a `(number) => string`, so
`numbers.map(someStr.charAt)` is a `string[]`. Conditioning each higher-order edge
on its callback type being independently reachable -- a fixpoint, since new edges
make new function types reachable -- still settles at 100% dense after three rounds,
with 146 function types obtainable from method references alone. And even without
`map`, `booleans.join(",").split(",")` walks from booleans to strings.

| | types | edges | density |
|---|---:|---:|---:|
| PLDI full environment | 396 | 2,371 | 44.9% |
| members within the six only | 101 | 621 | 40.9% |
| the fragment without higher-order methods | 125 | 766 | 76.0% |
| **the fragment as specified** | **153** | **934** | **100%** |

So in TypeScript essentially every type is convertible to every other, and a
reachability filter is vacuous. This explains an earlier measurement that looked
odd: of PLDI's 86,106 reachability queries on one program, 99.8% did no search work
and 75% had a goal of `unknown`. They were asking a question whose answer is almost
always yes.

**PLDI's own graph filters only because it is incomplete.** At 44.9% it rejects
pairs that are genuinely reachable in TypeScript, because its reachability ignores
generic instantiation (it handles that lazily during the parse instead) and because
`_reachable_bfs` gives up after 1000 iterations and reports "not reachable". Both
make it stricter than the language.

### Where the filtering actually comes from

If reachability admits everything, the constraint must bite elsewhere. Three places,
in order of how much they filter:

1. **Exact type at a completion point.** At `let s: string = x|` the token `;` is
   illegal, because ending here requires the expression to *be* a string, not merely
   to be convertible to one. This is where nearly all the type filtering lives.
2. **The member namespace after a dot.** At `someStr.|` only `string`'s members are
   legal -- a few dozen names out of everything in scope. Highly selective, and it
   depends on the receiver's type, so it cannot be precompiled per grammar position.
3. **Syntax**, which XGrammar already handles.

What reachability is still needed for is *not* rejecting: it is knowing that a
prefix is not yet doomed, so `x` may be written at a string position. Since the
answer is nearly always yes, that is cheap to answer and cheap to be right about.

So the per-type masks in DESIGN.md should be indexed by the exact required type at
completion points and by the receiver type at member positions -- not by the set of
types that reach a goal, which is every type.

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
