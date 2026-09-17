# How fast is the PLDI type checker?

Their system checks generated code for type errors while the LLM writes it. That
checking costs CPU time. This measures how much, and where it goes.

No GPU and no model weights are needed.

## The idea

The paper's saved results contain tens of thousands of TypeScript programs their LLM
already wrote. We extract those programs, feed each one to their checker, and time it.

This is a direct measurement of their code on real inputs.

## Setup

The checker needs Python 3.11 (it uses `typing.Self`).

```bash
python3.11 -m venv venv
./venv/bin/pip install frozenlist regex termcolor frozendict
```

## Step 1: build the corpus

```bash
python3 extract_corpus.py
```

Reads the 144 result files in the submodule and writes one `.ts` file per generated
program:

```
corpus/index.csv
corpus/programs/<model>/<c|nc>/<dataset>_<task>_s<seed>_<problem>.ts
```

30,023 programs, 10.3 MB of TypeScript. `index.csv` carries the metadata (length,
model, dataset, task, seed, constrained-or-not, whether tests passed), so programs
can be selected by size or model without opening them.

`c` means the program was generated with type-constraining on, `nc` means without.
Constrained programs are ones the checker accepts completely; unconstrained ones it
will often reject partway.

The corpus is gitignored — it regenerates from the submodule in seconds.

### What gets cut

Each saved record stores the LLM's program with the benchmark's own correctness tests
appended (`assert.deepEqual(candidate(99),101)` and so on). The LLM never wrote those
and the checker never saw them during generation, so everything from
`declare var require` onward is removed.

Also skipped: 1,350 runs where generation crashed, 7 that hit the 300s timeout, and
1,362 rows superseded by a later re-run. (Their experiment script resumes a cancelled
run by appending to the same file, so some problems appear twice; the paper's own
analysis keeps the last row, and so does this.)

## Step 2: time the checker

```bash
./venv/bin/python profile_advance.py        # 10 programs
./venv/bin/python profile_advance.py 60     # 60 programs
```

One line per program:

| Column | Meaning |
|---|---|
| `chars` | How many characters of the program the checker read |
| `seconds` | How long that took |
| `ms/char` | Milliseconds per character — the headline number |
| `states` | How many possible interpretations the checker held at the end |
| `note` | Flags a program the checker refused to finish reading |

`states` is the one to watch. The checker does not track a single reading of the code.
It tracks every reading still possible, because after seeing `foo(` it cannot yet know
whether that returns a number or a string. Each new character is checked against every
interpretation held, so this number acts as a multiplier on cost.

## What we found so far

Ten programs, on one desktop CPU:

- Nine ran at roughly **1 ms per character**.
- One ran at **51 ms per character**, about 60x slower, holding **656** interpretations
  at once. That single program was 96% of total time. Not a fluke: re-running gave
  51.1s and 53.1s with the machine otherwise idle.

So cost is usually small and occasionally enormous. A single average hides this and
should not be quoted alone.

A later spot check: a 5,755-character program ran at **0.65 ms/char with 8 states** —
cheaper per character than the short programs despite being 17x longer. So length alone
does not cause the blow-up; the number of live interpretations does. That is one data
point, not a conclusion.

## What is still unknown

Which *phase* inside the checker burns the time. The candidates are:

1. advancing the parser (reading a character, updating the live interpretations)
2. working out the types of expressions
3. the reachability search — "can I still reach the type I need from here?"
4. scope lookups — scanning the variables in scope

Running under `cProfile` and grouping functions by phase answers this. The phases call
each other, so the grouping must use each function's own time, not time including
what it called, or the same microsecond gets counted repeatedly.

## Caveats

- Timings come from this desktop, not the paper's machines.
- This measures the checker reading code that was already accepted. During real
  generation it also checks candidate tokens that get rejected, which is extra work
  not measured here.
