# Pre-registration: parser-advance cost verification

Written BEFORE running the harness. The harness script contains no prediction values.

## Question
What is the CPU cost of the PLDI parser advancing over already-accepted text?
No GPU, no model, no rejected candidates.

## Procedure (fixed before running)
- Input file: `results_paper/mbpp_google_gemma-2-2b-it_s=0_t=1_synth_c.jsonl`
- Selection: records with `crashed` in (None, "None") and `time_taken` < 295,
  sorted by `instance_id`, take the **first 10**.
- Text parsed: the `compilable` field, truncated to the **first 1000 characters**.
- Initial state: `custom_end_initial_state("```", {})` — same as the real constrained run.
- Advance: `incremental_ts_parse(state, text)`, timed with `perf_counter`.
- Chars counted: `len(result.parsed_code)` (chars actually consumed; parser may reject early).
- Two cache modes, both reported:
  - **cold**: `GLOBAL_REACHABLE_CACHE.clear()` before each instance
  - **warm**: never cleared, cache carries across instances (as in their run, which
    processes many instances in one process)
- Metric: **ms per character**, reported as median and mean over instances.
- Single run, no repeats, no tuning. Whatever it prints is the result.

## Prediction (stated before running)
The earlier regression over recorded results gave a constraint base cost of
`a = 27.9 ms` per unit of `T`, where `T = compilable_chars / 3.5`.
Expressed per compilable character that is `27.9 / 3.5 = 7.97 ms/char`.

That 7.97 covers parser advance **plus** detokenization **plus** the GPU sampling op.
So if parser advance dominates, the profiled number should be somewhat **below** 7.97
but the same order of magnitude.

- **Predicted range: 4–10 ms/char** (warm mode, median).
- **Falsified if: < 1 ms/char or > 30 ms/char.**
  Below 1 would mean the parser is not the dominant cost and the earlier breakdown is wrong.
  Above 30 would mean the fit badly underestimated parser cost.

## Known confounds (stated before running)
1. Different CPU from the paper's machines. Unknown factor, plausibly ~2x either way.
2. `compilable` includes appended test code that the model did not generate. The
   earlier regression used the same field, so the comparison is consistent, but neither
   number is "cost per generated character".
3. Truncating at 1000 chars biases toward early program positions, where scope is
   smaller and parsing is likely cheaper. Expect a slight underestimate.
4. Cold mode overestimates relative to their run; warm is the closer analogue.
5. Their run also parsed candidate tokens; this measures only the accepted path.
