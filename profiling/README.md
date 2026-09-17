# How fast is the PLDI type checker, and where does the time go?

Their system checks generated code for type errors while the LLM writes it. That
checking costs CPU time. This measures how much, and what it is spent on.

No GPU and no model weights are needed. Everything replays TypeScript their LLM
already wrote, saved in the paper's own results.

## Setup

The checker needs Python 3.11 (it uses `typing.Self`).

```bash
python3.11 -m venv venv
./venv/bin/pip install frozenlist regex termcolor frozendict
```

Add `py-spy` and `tokenizers` only if you want the cross-checks.

## The scripts

| Script | What it does |
|---|---|
| `extract_corpus.py` | Pulls every generated program out of the paper's results into `.ts` files |
| `profile_advance.py` | Times the checker on those programs |
| `analyze_results.py` | Summarizes a timing run |
| `profile_phases.py` | Splits the checker's time across its internal phases |
| `crosscheck_copying.py` | Re-measures the object-copying share without a profiler |
| `profile_states.py` | Measures how many readings of the code the checker holds, and how many are duplicates |

## Step 1: build the corpus

```bash
python3 extract_corpus.py
```

30,023 programs, 10.3 MB of TypeScript, in
`corpus/programs/<model>/<c|nc>/<dataset>_<task>_s<seed>_<problem>.ts`, with an
`index.csv` of metadata so programs can be selected without opening them.

`c` means generated with type-constraining on; `nc` without. Only `c` programs were
ever seen by the checker during generation, so those are the ones worth timing.

Excluded: the benchmark's own correctness tests, which are appended to every saved
program but were never written by the LLM (everything from `declare var require`
onward); runs that crashed (1,350) or timed out (7); and 1,362 rows superseded by a
later re-run, matching what the paper's own analysis does.

## Step 2: time the checker

```bash
./venv/bin/python profile_advance.py --jobs 8 --timeout 30
./venv/bin/python analyze_results.py results_quick.csv
```

Over 1,000 constrained programs:

| | ms per character |
|---|---:|
| median | 1.63 |
| 90th | 4.56 |
| 99th | 11.60 |
| max | 39.95 |

98.3% finish, **1.6% exceed 10 seconds**. Median cost is 0.46s per program.

For context, the paper's own Table 4 (regenerated from their data) reports that
constraining adds 20-60% to end-to-end generation, about 0.9-2.1s per instance. Our
accepted-path measurement is 0.3-0.5s, so it accounts for roughly a quarter to a half
of that; the rest is rejected candidates, which we measured separately.

## Step 3: phase breakdown

```bash
./venv/bin/python profile_phases.py --states 1001
```

| Phase | Share of checker time |
|---|---:|
| Building and copying state objects | ~45-50% |
| Parser advance | ~34% |
| Type derivation | ~12-14% |
| **Reachability search** | **~6%** |
| Scope lookup | ~2% |

Near-identical on a cheap and an expensive program: the slow ones do the same work
more times, not different work.

Cross-checked three ways, since `cProfile` overstates code that makes many small
calls. A timer wrapping only `dataclasses.replace` (7.6% overhead) puts it at 39.5%
of an uninstrumented run; `py-spy` sampling, with no instrumentation at all, agrees
once the generated constructors it calls are counted.

Concretely: a 467-character program makes **602,157 object copies**, about 1,290 per
character, at 3.03 us each. The parser states are frozen dataclasses, so every
character transition rebuilds them instead of mutating.

**The paper's actual algorithm is cheap; its data representation is expensive.**

## Step 4: the shape of the live readings

```bash
./venv/bin/python profile_states.py --states 1001 --stride 4 --max-signatures 20000
```

The checker never commits to one reading of the code. After `f(` it cannot know
whether the argument is a number, a string or another call, so it carries every
still-possible reading forward. Cost is roughly (readings) x (characters).

A "reading" is a whole root-to-leaf path: two leaves that look alike under different
parents are not interchangeable, because what may follow them differs.

Two duplication levels are measured. **L1** counts readings that are structurally
identical — merging those needs no argument at all. **L2** additionally ignores the
already-parsed history, which is what a merging parser could collapse, but that is a
claim rather than a fact.

| Program | peak readings | duplication (median) | max | work-weighted |
|---|---:|---:|---:|---:|
| cheap, gemma-2-2b | 264 | 1.00x | 1.33x | - |
| cheap, Qwen | 200 | 1.00x | 1.44x | - |
| **expensive, DeepSeek** | **6,272** | **2.00x** | **32x** | **3.29x** |
| **expensive, CodeLlama** | **3,200** | **4.00x** | **16x** | **5.65x** |

So on expensive programs **70-82% of the work goes to readings that are byte-for-byte
duplicates of another reading**. Cheap programs have essentially none.

Three further observations:

- **The ratios are powers of two.** 115 of 118 positions on one program and 147 of
  149 on the other have duplication of exactly 1x, 2x, 4x, 16x or 32x. That is a
  mechanism, not incidental overlap: the same reading is produced 2^k times by k
  repeated binary forks.
- **Where the duplication is created differs between programs.** At the busiest
  position of the first, 94% of the children at depth 8 are copies of a sibling, 47%
  at depth 9, and none below depth 10. The second has no duplicate siblings at any
  depth; its redundancy sits entirely at the root, where 1,620 of 1,728 top-level
  states are copies of another (108 distinct, matching its 16x peak). So there is no
  single fan-out site to point at, and a fix has to handle both.
- **Work is extremely concentrated.** Half of all readings sit in 5-8% of positions.
  The busiest position holds 6,272 readings that collapse to 196 distinct ones.

L1 and L2 are identical on the first program; on the second, L2 collapses about 1.9%
further (5,102 vs 5,198). So nearly all of the duplication is plain structural
identity, and dropping history buys little on top.

Whitespace characters multiply readings by roughly 24x on every program measured.

## What this means for the project

GOALS.md warns against claiming a speedup by rewriting Python in C++. Two findings
sharpen that warning:

1. About half the checker's time is frozen-dataclass allocation, removable with
   mutable state and rollback — which is what XGrammar already does.
2. On expensive programs, most of the remaining work is spent on readings that are
   exact duplicates of each other. Merging them is what an Earley or GLR parser does
   as a matter of course.

Neither is a new algorithmic idea, so a credible comparison has to be against a
baseline with both already fixed. What is left after that is the real question.

The type dimension looks small and precomputable: at most 2-3 distinct expected types
are ever in play at once, even when thousands of readings are alive. That is
encouraging for the split GOALS.md asks for, between fixed compiled information and a
small environment-dependent part.

## Metric bugs found and fixed along the way

Each of these made an earlier number wrong, and each is worth knowing before trusting
any of this:

- `len(parser_state)` counts only the top-level list, understating true readings by a
  median of 10x and up to **1568x**. Every state count quoted before `profile_states.py`
  used that biased proxy.
- Summing readings and distinct counts across a program and dividing once hides local
  duplication, which is exactly where cost concentrates. Ratios are per position now.
- Nearly every state carries a `typ` defaulting to `AnyPType` ("unknown"), so taking
  the innermost non-None type returned "unknown" almost always. Only
  `ExpressionParserState` sets a concrete type.
- A program ending with zero surviving readings was recorded as `ok` because only
  characters-read was checked. Zero readings means the checker rejected the text.
- There is deliberately no frontier-survival metric: signatures include fields that
  accumulate the consumed text, so overlap between consecutive positions is ~0 by
  construction and measures nothing.

## Caveats

- Timings come from one desktop, not the paper's machines.
- The phase breakdown is two programs; the reading-shape results are four.
- Rejection costs were measured with random vocabulary tokens, which die immediately.
  Real model proposals are plausible code that fails later, so those numbers likely
  understate.
- `profile_states.py` skips signature work above `--max-signatures` readings, so a
  cap set too low silently omits the busiest positions — which are the ones that
  matter.
