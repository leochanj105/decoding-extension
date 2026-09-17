# How fast is the PLDI type checker?

Their system checks generated code for type errors while the LLM writes it. That
checking costs CPU time. This measures how much.

No GPU and no model weights are needed.

## The idea

The paper's saved results contain thousands of TypeScript programs their LLM already
wrote. We take those programs, feed each one to their checker, and time it.

This is a direct measurement of real code on real inputs.

## Running it

The checker needs Python 3.11 (it uses `typing.Self`).

```bash
python3.11 -m venv venv
./venv/bin/pip install frozenlist regex termcolor frozendict
./venv/bin/python profile_advance.py        # 10 programs
./venv/bin/python profile_advance.py 60     # 60 programs
```

## Reading the output

One line per program:

| Column | Meaning |
|---|---|
| `chars` | How many characters of the program the checker read |
| `seconds` | How long that took |
| `ms/char` | Milliseconds per character — the headline number |
| `states` | How many possible interpretations the checker was holding at the end |
| `note` | Flags a program the checker refused to finish reading |

`states` is the interesting one. The checker does not track a single reading of the
code. It tracks every reading still possible, because after seeing `foo(` it cannot
yet know whether that returns a number or a string. Each new character must be
checked against every interpretation being held. So when this number grows, the
checker slows down proportionally.

## What we found so far

Ten programs, on one desktop CPU:

- Nine ran at roughly **1 ms per character**.
- One ran at **51 ms per character** — about 60x slower. It was holding **656**
  interpretations at once. That single program was 96% of the total time.

It was not a fluke: re-running it twice gave 51.1s and 53.1s with the machine
otherwise idle.

So the cost is usually small and occasionally enormous. A single average figure hides
this and should not be quoted on its own.

## What is still unknown

The nine fast programs were all short, 120-340 characters. The slow one was the only
long one. So we cannot yet tell the difference between:

- that one program being unusual, versus
- every program becoming slow once it gets long enough.

Running more programs answers this. That is the only reason to pass a larger number
to the script.

## Two things this script fixes

An earlier version of this measurement had two flaws, both corrected here:

1. **It stopped after 1000 characters** to bound runtime. The slow program hit that
   limit, so its real cost was never measured. There is no limit now.
2. **It fed in text the LLM never wrote.** The saved `compilable` field is the LLM's
   program with the benchmark's own correctness tests appended — roughly half the
   text. The script now cuts at `declare var require`, where those tests begin.

## Caveats

- Timings come from this desktop, not the paper's machines.
- This measures the checker reading code that was already accepted. During real
  generation it also checks candidate tokens that get rejected, which is extra work
  not measured here.
