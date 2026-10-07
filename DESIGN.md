# Design: type constraints as token masks

How to answer "which next tokens have a well-typed completion?" with a lookup
instead of a search, and what of XGrammar can be reused to do it.

Status: design agreed, not implemented. Every number below is measured on this
desktop; the sources are named so each can be rechecked.

## The query being replaced

PLDI TCD decides identifier legality in `parser_ts.py:138`:

```python
typs = set()
for v, (t, _) in self.identifiers.items():
    if v.startswith(self.id_name) and v in self.whitelist:
        typs.add(t)
return any_reachable(typs, goal, min_operator_precedence, ...)
```

In scope, filtered by the partial name typed so far, types collected, "can any of
these still reach the goal?". It returns one boolean, and it is called **once per
candidate token**.

That call volume is the entire cost. On the heaviest program in the corpus
(467 chars, 123 tokens) the checker answers **86,106 `reachable` calls** that
reduce to **76 distinct `(type, goal)` pairs**, with **99.8% doing no search work
at all** — the global cache already holds the answer. Reachability search is only
~6% of checker time; building and copying parser state objects is 45-50%.

So the target is not a faster search. It is asking ~40 type-level questions per
step instead of ~151,665 token-level ones, and turning the answer into a mask.

## Why the type layer is easier than it looks

`OPERATOR_REACHABLE_TYPE_MAP` (`types_ts.py:2288`) is eight functions, and each is
a pure function of the type object:

```python
def call_reachable_types(typ):           # "p()"
    if isinstance(typ, FunctionPType): return {typ.return_type}
    return {}

def member_access_reachable_types(typ):  # "p.x"
    return set(x[0] for x in typ.attributes.values())
```

Declaring `fun f(number) -> string` therefore adds **no edge** to the type graph.
The edge `(number)=>string ⇝ string` is intrinsic to the function *type*, present
whether or not a symbol has it. `.length` belongs to `StringPType`, not to the
environment.

Two consequences that shape the whole design:

- **The environment contributes no edges, only a seed set** — which types are
  inhabited by something currently in scope.
- **Scope exit cannot delete an edge**, because declarations never added one.
  `.forEach` dropping 36 symbols shrinks the seed set, not the graph. The type
  graph is insert-only, so no closure needs repairing.

The type universe is demand-driven: start from primitives and built-ins, and add a
type when the program actually constructs or mentions it. The search's depth budget
is seeded from the nesting depths of the types already in play (`reachable()`, the
`max_depth` preamble), so it never invents types deeper than the program uses.

## Three parts, by how often each changes

Nothing recomputes what a slower-changing part already knows.

| part | what it is | size | changes when |
|---|---|---|---|
| **per-type masks** | for each type, the tokens that can start an in-scope identifier of that type | median **4** tokens, max **19** | a declaration adds or removes a symbol of that type |
| **context filter** | the set of *types* usable at this position | 17-44 types = 1-2 words | the parser moves |
| **per-step merge** | context filter ∩ live types, then the per-type masks unioned | median **92** tokens, max **177** | every step |

Worked example, at `let s: string = ▮`. The per-type masks already hold, for
`string`, the tokens that can start `msg`, `name`, … . The context filter says
which types work here: `string` directly, `number` via `+`, any function type
returning `string` via a call. The merge intersects that with what is in scope,
unions those types' token lists, and ORs in the static syntax mask (string-literal
opening, `(`, and so on).

The context filter is where the precedence window and the `in_array` /
`in_nested_expression` / `in_pattern` stacks live. They never touch tokens — they only decide which *types*
are usable — so they are applied at type granularity, on a one-word bitset, and
tokens are materialized once afterwards. Narrowing a precedence window strictly
shrinks `allowed_ops` and so strictly shrinks the usable set, which makes nested
contexts nested sets: a narrower context can be stored as a wider one minus a
delta, again at type granularity.

Reachability runs **forward**, not backward. For each of the ~40 live types, ask
"usable here?" using the relation PLDI already has, including pushing a candidate
type forward through a generic-argument frame, which is exact and cheap. Nothing
has to be inverted.

## Measured sizes

`profiling/measure_symbol_masks.py`, heavy program, Qwen2.5 vocab (151,665 tokens,
150,217 after dropping special tokens), 123 steps:

| | |
|---|---:|
| live symbols | 24 → 72 |
| distinct types among them | 17 - 44 |
| per-type mask, median / p99 / max | **4 / 15 / 19** tokens |
| type masks exceeding XGrammar's dense threshold of 1000 | **0 of 2,478** |
| union over all live types, median / max | **92 / 177** tokens |
| same, counting space-prefixed token forms | 184 / 353 |
| a dense bitmask, for comparison | 18,780 bytes |

So the per-type masks are **sparse, always**. 92 token ids is 368 bytes against 18,780 dense —
51× smaller — and merging ~40 sorted lists of ~4 entries is a few hundred
insertions. An earlier worry that a 40-way union of dense masks would cost 4-60 µs
was an artifact of the wrong representation; in this regime it does not arise.

Names collapse hard under shared prefixes: 72 symbols yield only 92 distinct
startable tokens, because the built-ins (`forEach`, `filter`, `find`, …) share
their opening bytes.

## The A/B/C split GOALS.md asked for

**A. Fully static** — compiled once from the language and tokenizer.
Syntax masks per grammar position; the per-precedence-window operator tables; the
per-type masks for built-in symbols; and — for the fragment in FRAGMENT.md — the
*entire* type reachability relation.

That last one is static, and does real but partial work.

The universe is finite and known before generation starts -- 125 types, 750 edges, a
4 KB table closed in 4 rounds in 20 ms, for every program -- and 57.6% of pairs are
reachable, against 44.9% for PLDI's full environment. So it excludes about 42% of
pairs: real work, but not the selective part.

The rule that makes it correct: **calling a function requires its arguments to be
writable.** With no arrow functions, `string[].reduce` cannot fire because its
callback is not obtainable. Ungated, every type reaches every other and the filter is
vacuous.

Most selectivity comes from two other places, and the per-type masks should be
indexed by these:

1. **The exact required type at a completion point.** At `let s: string = x|` the
   token `;` is illegal: ending here requires the expression to *be* a string, not
   merely to be convertible to one. Nearly all type filtering lives here.
2. **The receiver's members after a dot.** At `someStr.|` only `string`'s members
   are legal. Highly selective, and dependent on the receiver's type, so it cannot
   be precompiled per grammar position.

Reachability then answers "is this prefix doomed yet?" and rules out the 42%.

**B. Environment-dependent** — recomputed when a declaration lands.
One per-type mask, for the one affected type. Nothing else: no closure update, no
grammar recompilation.

**C. Per-token** — genuinely unavoidable.
The context filter composed for the current position (a few one-word ops),
intersected with the live types, the per-type masks merged, unioned with the static
syntax mask.

Nothing in group B recompiles anything. A declaration rewrites one per-type token
list; the grammar, the FSMs and the compiled mask cache are untouched.

This is worth stating because the obvious alternative is to skip all of this
and bake the environment into the EBNF as alternatives:

```
ref_string  ::= "s" | "t" | "msg"
call_string ::= ("f" | "g") "(" expr_number ")"
```

then regenerate and recompile whenever a declaration lands. GOALS.md requires
ruling this out before inventing a mechanism, so it was measured: **3-9 ms** per
recompile for 6-768 symbols, against a mask fill of **0.7-2.0 µs** on the same
grammars — roughly 1,000× too slow per declaration. XGrammar's `RuleLevelCache`
does not help; a recompile after adding two symbols costs the same as a cold
compile. (Why it does not help is unverified; grammar parsing, optimization and
per-rule FSM construction plausibly dominate the mask computation the cache
covers.)

So unmodified XGrammar cannot carry a changing environment. That is what the
measurement establishes — not that this design beats recompiling inside itself,
where recompiling never occurs.

## What we reuse from XGrammar

Verified by reading the source and building the library (`cmake` with
`XGRAMMAR_BUILD_PYTHON_BINDINGS=OFF`, no torch needed; note `cmake/config.cmake`
shadows that flag with a plain `set()`, so it must be overridden from the build
directory).

| what | where | why it fits |
|---|---|---|
| `AdaptiveTokenMask` | `cpp/compiled_grammar_impl.h` | Already the exact per-type-mask representation: `kAccepted` / `kRejected` / `kAcceptedBitset`, switching at `USE_BITSET_THRESHOLD = 1000`. Our masks max out at 19, so always the sparse arm. |
| `TokenizerInfo` | `cpp/tokenizer_info.cc` | Vocab decoding, lexicographically sorted vocab, `GetTrieSubtreeNodesRange`, `token_id_to_sorted_vocab_index`. Maps name prefixes to token ids, which is all the per-type masks need. |
| mask cache keyed by grammar position | `StateHashForCache`, `cpp/earley_parser.h` | Keyed on `(rule_id, sequence_id, element_id, sub_element_id)` — exactly the static syntax part. |
| `FillBitmaskForStates` | `cpp/grammar_matcher.cc:1851` | Union over live Earley states plus resolution of "uncertain" tokens. The loop we extend, not replace. |
| mutable parser state with rollback | `PopLastStates`, `cpp/earley_parser.cc` | Removes PLDI's dominant cost directly: 45-50% of its time is frozen-dataclass copying, 602,157 object copies on a 467-char program. |
| `DynamicBitset` | `cpp/support/dynamic_bitset.h` | The context filter's type bitsets, and the final mask. |
| Earley parsing with state merging | `cpp/earley_parser.cc` | Collapses the duplicate readings that cost PLDI 70-82% of its work on expensive programs. |

What has to be written: the type universe and its forward reachability with
context (a port of `types_ts.py` logic, not a new algorithm); the type→symbol index
with scoped push/pop; per-type mask construction from names via `TokenizerInfo`.

## Where the required type comes from

At `s = ▮` the parser state says "I am in rule `expr`, element 0" and nothing
more. The mask we want depends on something that state does not record: `s` is a
string, so this position must produce a string. `expr` is one rule used at every
expression position and says nothing about types; the type came from `s`'s
declaration, which the parser read and forgot.

Two ways to get the type to that position:

**Rule per type** — write `expr_string`, `expr_number`, … so the required type is
implied by *which rule you are in*. Needs no change to XGrammar at all. Measured
working (the compile and mask-fill numbers above came from grammars of this shape),
but it needs one rule per type, so it cannot survive an open type universe.

**A field in the parser state** — keep one generic `expr` rule and attach the
requirement to the state. **This is the chosen approach.**

### It is a field of a kind XGrammar already has

`ParserState` is not purely positional. It already carries context that is set when
a rule starts, inherited by everything inside it, and consulted when the mask is
built:

```
budget_deadline             tokens this rule occurrence may still consume
char_budget_deadline        characters it may still consume
active_temperature_rule_id  whose sampling temperature applies
```

We add a fourth of the same kind. It is deliberately **not** called a type:

```
requirement_id              which requirement this position must satisfy
```

an index into a table we maintain.

### Why it must live in the state

The cheaper-looking option is a side stack holding "currently required type". It
breaks, because this is an Earley parser carrying many readings at once: after `f(`
it does not know whether the argument is a number, a string or another call, so all
of them stay alive. Two readings can sit at the same syntactic position while
requiring different types, and a side stack cannot say which reading a requirement
belongs to. In the state, each reading keeps its own.

### Why it does not blow up the precomputed cache

`StateHashForCache` deliberately ignores most fields — only rule, sequence,
element and sub-element — because a syntactic position's mask does not depend on
budgets or temperature. **We keep ignoring the requirement there too.** Every
ordinary position (keywords, punctuation, literals, brackets) keeps its compiled
mask untouched.

The requirement is read at exactly one place: the runtime-lexicon element, where
there is no compiled mask anyway. So the cache is not multiplied by the number of
requirements, which is what would have made this unaffordable.

### What it does cost

`StateHashForParsing` compares every field, so two readings at one syntactic
position with different requirements will no longer merge. More live states. That
is the real price, and it is semantically right — different tokens are legal in
each.

### Why "requirement" and not "type"

The field names an entry in a table whose contents XGrammar never inspects. Three
operations define it:

| operation | for types | for a refinement, e.g. `X+Y<=20` with `X=10` |
|---|---|---|
| **specialize** given context | "same type as `s`" → `string` | → `Y<=10` |
| **admits** a candidate? | can this symbol's type reach `string`? | does `Y`'s range satisfy `Y<=10`? |
| **name** it, for caching | a type id | the formula, interned |

The mask layer needs only the third plus a loop over the second, and mentions
types nowhere. Rule-per-type could never express `Y<=10` — it would need a rule per
formula — so this is the second reason to prefer the state field.

Honest limits of the generalization: with types there are 17-44 live requirements
and masks are reused heavily, whereas `X=10` and `X=11` yield different residual
requirements, so the cache could miss every step. The structure survives (~40
`admits` calls per step) but the saved work does not. And `admits` is a cached
lookup for types against a solver call for formulas.

## Mask generation is lookup, with a small trial pass

For each grammar position the compiler splits the vocabulary three ways
(`compiled_grammar_impl.h`):

```
accepted    definitely allowed here
rejected    definitely not allowed here
uncertain   cannot be judged from this position alone
```

Accepted and rejected are pure lookup. **Uncertain** exists because a token can be
longer than the rule it starts in: the compiler runs per position, blind to what
encloses it, so a token that finishes the current rule and keeps going (`"abc") ;`)
cannot be judged there. Only those are resolved at runtime, by feeding their bytes
into the parser and rewinding.

How much trial that actually is, measured on the typed grammars above:

| | 12 symbols | 48 | 192 |
|---|---:|---:|---:|
| compiled positions | 99 | 211 | 395 |
| **positions with zero uncertain tokens** | **63%** | **65%** | **81%** |
| uncertain per position, median | **0** | **0** | **0** |
| mean / p90 / max | 81 / 353 / 1447 | 86 / 353 / 1447 | 50 / 85 / 1447 |
| positions stored as a dense bitset | 1 | 1 | 1 |

Out of 151,665 tokens. So most positions trial nothing, and the worst trials under
1% of the vocabulary — the string-literal interior, where many tokens could close
the quote and run on.

### Rewinding

The parse is stored as one row of states per character consumed. Advancing appends
a row; rewinding *n* characters truncates *n* rows. Undo is a truncation, which is
why it is nearly free — and the direct contrast with PLDI, which rebuilds frozen
objects every character and truncates nothing (45-50% of its time). Trials walk
candidates in lexicographic order so neighbours share prefixes and only the
differing suffix is rewound (`prev_matched_size` in `FillBitmaskForStates`).

### What it means for the symbol table

Our symbol table is a second piece of state that must stay consistent with the
parser. Trials only ever need to *read* it, never write it: a declaration becomes
real only when a token is committed. XGrammar already draws this line with
`capture_recording_`, enabled "during definitive advances (accepting a token or
string), not during speculative exploration (mask computation, jump-forward search,
lookahead checks)".

So: **consult during speculation, mutate only on a committed accept.** Given that
most positions speculate over nothing at all, the table's undo is a rare
correctness detail rather than a hot path.

Edge case left open: a single token that both completes a declaration and uses the
name it declares. Skipping the update during speculation is wrong there. Rare
enough to defer, but it is a real hole.

## The one extension XGrammar needs

XGrammar's mask cache assumes the mask at a grammar position is **static**. That
holds for every position here except one: the identifier.

Three changes, kept as small as possible. New code goes in new files
(`cpp/dynamic_lexicon.{h,cc}`); edits to existing files are held to a single field
and a single branch, because upstream XGrammar is active and every added line in an
existing file is a future merge conflict.

The cheapest thing that could work needs no extension: let XGrammar mask a generic
identifier (`[a-zA-Z_][a-zA-Z0-9_]*`), compute our type-filtered mask separately,
and intersect the two outside XGrammar. **Measurement rules this out** — see
"straddling tokens" below. 83% of identifier uses are entered by a token that also
carries the preceding delimiter, so a per-type mask holds `x` while the model needs `(x`.

Indexing the prefixed forms too (` x`, `.x`, `(x`, `[x`) would cover 99% of those
but is unsound: inside a token like `(x`, the identifier position falls *after* the
`(` is consumed, and the goal type there is set by what that `(` opened — not by
the position the mask was computed for. Resolving it means locating the identifier
inside the token, which is parsing the token.

So the extension is needed: a grammar element meaning *"an identifier drawn from a
runtime-supplied, type-filtered lexicon"*, and a branch in `FillBitmaskForStates`
that consults the runtime index when the byte-wise advance reaches such an element.
XGrammar already walks uncertain tokens byte by byte, so this rides on existing
machinery. Not a recompile and not a new parser.

Everything else — literals, keywords, punctuation, nesting — stays compiled and
untouched.

## The target: requirements carried on grammar elements

The design above gets the required type to the identifier position by one of two
routes. The one that scales is to carry the requirement on the parser state, and the
general form of that is worth writing down now because it shapes the extension.

**Each grammar element carries a residual requirement, and an expansion is allowed
only if the requirement can still be cleared.**

That is constraint logic programming with tabling, and the correspondence is exact
enough to be useful:

| CLP | here |
|---|---|
| a goal plus a constraint store | a grammar element plus its residual requirement |
| a derivation step | expanding a grammar rule |
| the store stays satisfiable | the requirement can still be cleared |
| tabling a subgoal | memoising (element, requirement) -> allowed tokens |

The last row is the important one: **the table is the mask cache.** XGrammar already
caches masks keyed by grammar position; this keys them by position *and* residual
requirement. Whether that is affordable is an empirical question, and the answer
looks good: on one program PLDI's 86,106 reachability queries collapsed to 76
distinct (type, goal) pairs and 253 distinct full keys. A few hundred table entries
per program is nothing.

Two properties of type requirements, both measured, make them well behaved here:

- **At a fresh position the store is always satisfiable.** Every type in the
  universe is constructible from literals, so a requirement can always be cleared
  and no expansion is ever blocked for want of an inhabitant.
- **Committing narrows it.** Once a sub-expression has a type, 42% of goals become
  unreachable, so the store does real pruning from the first token onward.

Generalising past types is then a matter of what the residual is. For a refinement
like `X + Y <= 20` with `X = 10` the residual is `Y <= 10`, clearing is satisfiability,
and the table keys on the formula. The machinery does not change; only the hit rate
and the cost of deciding "can this be cleared" do.

## Building it: the dynamic lexicon first

The first increment is the identifier placeholder alone, with the required type taken
from the grammar position (`let x : T = ...`) rather than from a requirement field.
That exercises the hook against XGrammar's real mask code before any state is
carried.

XGrammar turns out to contain the needed mechanism already. `grammar_matcher.cc:840`
takes a state, maps its `rule_start_pos` to a byte offset, slices the input from
there to the current end -- correctly spanning both committed `accepted_bytes_` and
the speculative `temporary_input_bytes_` used while trialling a token -- and walks
those bytes through an FSM, caching progress per rule occurrence. A dynamic lexicon
is that same shape with a trie of permitted names in place of the FSM.

Three pieces, in order:

1. A registry mapping a tag to the names currently permitted for it, and the mask for
   a given (tag, prefix). New file, no XGrammar coupling beyond `TokenizerInfo`.
2. Recognition of lexicon rules by name convention (`__lex_<tag>`), resolved to rule
   ids at compile time. A convention avoids touching the grammar syntax, parser,
   printer and serialiser for a prototype.
3. A branch in `FillBitmaskForStates`, plus `ShouldTrackAcceptedBytes()` gaining a
   disjunct so byte tracking is enabled only when a lexicon rule is present.

## Unresolved

- **Mid-identifier positions.** The per-type masks as measured cover identifier
  *start*. Once
  bytes have been emitted, the legal set is "names of type τ with this prefix".
  Decided: enumerate directly from the few surviving names, and only add a
  prefix-keyed cache if it shows up in a profile.
- ~~**Straddling tokens.**~~ **Resolved by measurement** —
  `profiling/measure_straddle.py`, 800 constrained programs, 20,650 identifier
  uses. Tokens that finish a name and keep going (`s);`) are **0.6%**, median 0.0%
  per program, and 71% of those are a trailing `{`. Negligible.

  Tokens that *begin* before the name are **83.2%**, preceded by a space (72%),
  `.` (11%), `(` (11%) or `[` (5%). Byte-level BPE puts almost no token boundary
  where the type system's boundary is. This is what forces the in-matcher hook, and
  it is the layering mismatch stated concretely: 1.34 tokens per identifier, and
  five of every six identifiers entered mid-token.
- **Space-prefixed token forms** double the union (92 → 184). Whether both forms
  are reachable at a given position depends on the preceding whitespace, which is
  grammar state, not type state.
- **The generic-argument frame prunes symbols.** At `xs.map(▮)` with goal
  `number[]`, an in-scope `f : (string) => boolean` must be masked out while an
  arrow literal stays legal. Running the frame forward per candidate type handles
  this, but it means the frame is not a no-op for the symbol mask.
- **Soundness direction.** Any over-approximation of reachability admits a token
  with no well-typed completion, and the generator can then reach a dead end.
  Under-approximation only blocks legal programs. Approximations must be chosen
  deliberately and on the under- side.

## First milestone

Smallest thing that validates the claim: replay one recorded program's tokens and,
at every identifier position, produce the token mask through the three parts above,
checking it equals the set PLDI's `derivable` accepts when asked token by token.
Equality on the accepted path plus a sample of rejected tokens is the correctness
bar; mask-construction latency against the 44.5 ms/token mean (9.8 ms median,
instrumented, accepted path only) of the same replay is the performance bar.

That needs the per-type masks and the merge only. The context filter can start as
"ask PLDI's own `any_reachable` once per live type" — still ~40 calls instead of ~151,665 — which
isolates whether the mask construction is right before any reachability
precomputation exists.

## Caveats

- One program, one desktop. Symbol and mask statistics come from the heaviest
  program in the corpus; the cheap ones have ~200 readings rather than 6,272.
- The 44.5 ms/token baseline is measured under query instrumentation and counts
  only the accepted path, excluding rejected-candidate work. It is not a clean
  baseline, it is the order of magnitude.
- Per-type mask sizes assume a token may start a name iff its text is a prefix of that
  name. Tokens that span a name boundary are excluded and counted separately above.
- 150,217 of 151,665 vocabulary entries decode to UTF-8 text; the remainder are
  special or added tokens and were skipped.
