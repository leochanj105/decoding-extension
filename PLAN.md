# Plan

Where this is going, in order, with a check at each step that can fail.

Settled already: the design is in DESIGN.md, the fragment in FRAGMENT.md, and
`xgrammar/` is on branch `typed-lexicon` with no code on it yet.

## P0 — Write down what "legal" means, independent of PLDI

**A token is legal at a prefix if some completion exists that is syntactically
valid and well-typed under the fragment's rules.** One page stating that precisely,
plus which direction we err in when we cannot decide.

This has to come first because PLDI is currently both our answer key and our
target, and those must be separated. Once our own specification exists, a
disagreement with PLDI can be attributed rather than assumed to be our bug.

There is one concrete disagreement to expect. PLDI's reachability is bounded by
`while queue and i < 1000` in `_reachable_bfs` and by a nesting-depth filter
carried along the search path, and on hitting the cap it returns "not reachable",
so it silently rejects legal programs on large searches. (`max_steps` is *not* the
bound it looks like: `MAX_EXPRESSION_COMPLEXITY = math.inf`, so the `=5` default
never applies from the parser.) Neither bound exists in our design -- see P1 --
so our mask will sometimes be more permissive, correctly.

## P1 — Type reachability, which turns out to be fully static

For this fragment the type universe is **finite and known before generation
starts**: every type is built from the six base types, `declare function`
signatures are unary (so at most 36 function types), and the member tables are
fixed. Nothing about the universe depends on what a program declares.

So reachability is not environment-dependent at all. Close the universe, compute
the transitive closure once, and every later question is a bit test. Measured on
the fragment's graph:

| | depth <= 1 | depth <= 2 |
|---|---:|---:|
| types | 102 | 239 |
| distinct edges | 585 | 1,405 |
| build the graph | 2.5 ms | 4.7 ms |
| fixpoint | 3.5 ms | 18.5 ms |
| rounds to converge | 4 | 4 |
| stored as bitsets | 1.3 KB | 7 KB |

A 7 KB table, computed once in 18 ms of Python, for all programs. Four rounds to
converge.

This is why neither of PLDI's bounds is needed. They exist because PLDI's universe
is *open* -- it constructs `ArrayPType(t)` mid-search -- so the depth filter and
iteration cap are termination hacks, and it searches per query instead of computing
the relation once. With a closed universe the whole problem disappears, and with it
the path-dependence that made the relation non-compositional.

Remaining work: port PLDI's member tables for the six types, and decide how far out
to port them. The measurement charts that tradeoff -- members whose signatures
mention types outside the six either pull those types in (toward 239) or are left as
nodes with no outgoing edges (fewer edges). Demand-driven interning is *not* needed
here; it becomes necessary only if the universe is later opened by object literals
or generics.

**Check:** the ported tables reproduce the measured edge counts, and the closure
contains no pair PLDI calls unreachable except where P0 says we differ.

## P2 — The environment

The symbol table: names in scope with their types, an index from type to names, and
undo. Undo is needed because the parser rewinds, though rarely: median uncertain
tokens per position is 0 and 63-81% of positions have none, so the rule is to
consult during speculation and mutate only on a committed token.

**Check:** replaying a program, the symbol table matches what PLDI's parser holds
at the same point (`live_symbols` in `profiling/profile_reachability.py` reads it).

## P3 — The mask

Per-type token lists, and the merge. Measured sizes say these are small: median 4
tokens per type, 92 for the union, never near XGrammar's 1000-token dense
threshold.

The context filter starts as a call to PLDI's own `any_reachable`, once per live
type rather than once per candidate token -- about 40 calls instead of 700. That
isolates the mask construction from the reachability, so a wrong answer can only be
one of the two.

**Check:** `maskgen/replay_corpus.py` already establishes ground truth token by
token over the corpus (`c` 99.3% accepted, `nc` 37.3% rejected). Our mask must
admit every token of a `c` program and block somewhere in the `nc` programs that
PLDI refuses.

## P4 — Our own reachability

Replace `any_reachable`.

**Check:** identical answers on the 120,935 queries already recorded in
`profiling/reach_heavy_queries.csv`, except where our specification deliberately
differs -- each such case named.

## P5 — XGrammar integration

On `typed-lexicon`: the `requirement_id` field in the parser state, a grammar
element for a runtime-supplied lexicon, and the branch in `FillBitmaskForStates`
that consults our index for it. New code in new files; edits to existing files held
to one field and one branch, since upstream is active.

**Check:** the end-to-end mask equals the Python reference from P3.

## P6 — Measurement

Overhead against **syntax-only XGrammar on identical traces**, not against PLDI.
Comparing to PLDI confounds the algorithm with Python-versus-C++ and with their
representation choices, which GOALS.md explicitly forbids claiming credit for.
Same tokenizer, same implementation, one variable: the type reasoning.

Report the worst step as well as the mean, since one slow step stalls a pipeline.

A note on the premise: if mask construction lands in microseconds, as the measured
sizes suggest, there is no latency left to hide behind a forward pass. The
constraint stops being a latency problem rather than becoming an overlapped one.
Worth settling before building speculative overlap machinery.

## Deliberate departures from PLDI

| theirs | ours | why |
|---|---|---|
| open type universe, constructed during the search | closed universe, known before generation | removes the need for any bound at all |
| nesting-depth budget carried along the search path | none | the budget existed only to tame the open universe; it also made the relation non-compositional |
| hard 1000-iteration cap returning "not reachable" | none | theirs silently blocks legal programs |
| reachability searched per query | one fixpoint, 7 KB, then bit tests | 4 rounds to converge; nothing to search |
| frozen dataclasses, no state merging | XGrammar's mutable parser with rollback | removes 45-50% copying and the 70-82% duplicate readings |
| lazy: test top-k candidates in probability order | one eager mask | the entire point |
| three context stacks drained array, then paren, then pattern, regardless of real nesting | verify before copying | looks like a simplification that may be wrong on `[ (x) ]` versus `( [x] )` |

## Not yet decided

- Whether the fragment's own `+` and `==` need PLDI's full precedence machinery or
  a two-level window.
- How the goal type is computed for each requirement-setting rule, concretely.
- Whether to add one or two of PLDI's global objects to close the 40% graph gap.
- Testing strategy beyond the corpus walk, deferred on purpose.
