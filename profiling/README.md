# CPU-side profiling of the PLDI'25 type-constrained decoder

Goal: find where CPU time goes at each decoding step, after the LLM produces logits.
Everything here runs **without a GPU and without model weights**.

## Files

| File | What it is |
|---|---|
| `PREREGISTRATION.md` | Procedure + prediction, written **before** running, so the measurement could not be tuned to the expected answer |
| `profile_advance.py` | Replays recorded programs through the unmodified parser and times it. Exactly the version that produced `advance_log.txt` |
| `advance_log.txt` | Raw output of that run |
| `fit_cost_model.py` | Fits a per-token cost model to the paper's recorded `results_paper/*.jsonl` runtimes |

## Setup

The parser needs Python 3.11 (it uses `typing.Self`). No torch, no GPU.

```bash
python3.11 -m venv venv && ./venv/bin/pip install frozenlist regex termcolor frozendict
./venv/bin/python profile_advance.py
python3 fit_cost_model.py
```

## What we found

**1. Rejected candidates are a minority of the cost.** From the recorded `resamples`
field: only 4.4% of decoding steps reject anything, but those steps reject a lot —
12.4M rejected checks against ~1.5M accepted tokens, a ratio of 8.3:1. The fitted cost
per rejection is ~0.6 ms, putting rejections at roughly 15-20% of constraint cost.

**2. The full-vocabulary sampling op is not the bottleneck.** `sampling.py` draws
`multinomial` over the entire vocabulary every step. But fitted constraint cost per
token stays in the 36-62 ms range across models whose vocabularies differ 8x
(CodeLlama 32k = 41.8, Gemma 256k = 50.9). A vocabulary-sized op would scale; it doesn't.

**3. Parser advance dominates — and it is wildly heavy-tailed.** Replaying 10 programs:
9 ran at ~1 ms/char, 1 ran at 51 ms/char. That one instance was 96% of total time and
reproduces (51.1s, 53.1s on re-run, load average ~1.0). The cause is the number of
simultaneous parse interpretations the parser keeps alive: 0 for the fast programs,
**656** for the slow one. Every character is checked against every live state.

**4. Two independent estimates agree at the aggregate level.**

| Source | ms per generated char |
|---|---:|
| Replaying the parser on this machine | 16.8 |
| Fitted from their recorded GPU-run wall-clock times | ~14 |

These share no inputs, so the agreement is meaningful — but it rests on one dominating
instance, so treat it as provisional.

**5. The global reachability cache barely matters here.** Cold vs warm differed by 3.5%.

## Correction to the pre-registration

`PREREGISTRATION.md` predicted 4-10 ms/char. That band was **mis-derived**: it used the
`compilable` field's length, which turns out to be ~56% appended test-harness code the
model never generated. Correcting for that raises the prediction to ~14 ms/char, which
the measured aggregate (16.8) matches. The error was found during the run, not after,
and the prediction is left unedited in the file as written.

Note the measured **median** is 0.85 ms/char, far below both. Median and aggregate
disagree by 20x because the distribution is dominated by rare expensive positions.
Quoting a single "ms per token" number for this system is misleading.

## Open question

Is the 656-state blow-up a rare pathological program, or does every program reach it
once long enough? The 9 fast programs were short (120-340 chars); the slow one was the
only one exceeding 1000 chars, and it hit the script's cap so its true cost is still
unmeasured.

To answer: raise `N_INSTANCES` to ~60, remove `MAX_CHARS`, and stop parsing at
`TEST_MARKER` so exactly the generated program is measured.

## Caveats

- Token counts are estimated as characters / 3.5; exact counts are not stored.
- This machine's CPU differs from the paper's.
- `fit_cost_model.py` infers constraint cost from a difference of wall-clock totals, so
  it absorbs any other constrained-vs-unconstrained difference. The gemma-2 runs set
  `use_cache=False` (`sampling.py:144`), making model time quadratic in length; the
  script offers a quadratic term to check that this is not what `a` is absorbing.
- The quadratic variant is itself unreliable: for gemma-2-27b it fits a **negative**
  forward cost (m = -31.9 ms/tok), which is physically impossible and indicates
  collinearity between T and T^2 at these sample sizes. Treat `a` as an order-of-
  magnitude estimate, not a precise value. The direct replay in `profile_advance.py`
  is the more trustworthy measurement.
