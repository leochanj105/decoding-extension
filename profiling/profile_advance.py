"""Measure CPU cost of the PLDI parser advancing over accepted text.

No GPU, no model, no rejected candidates. Replays recorded programs through the
unmodified parser and reports milliseconds per character.
"""

import json
import statistics
import sys
import time

REPO = "/home/leochanj/Desktop/decoding-extension/type-constrained-code-generation"
sys.path.insert(0, REPO)

RESULTS = (
    REPO
    + "/experiments/main/results_paper/mbpp_google_gemma-2-2b-it_s=0_t=1_synth_c.jsonl"
)
N_INSTANCES = 10
MAX_CHARS = 1000
END_MARKER = "```"

from typesafe_llm.parser import types_ts
from typesafe_llm.parser.parser_ts import (
    custom_end_initial_state,
    incremental_ts_parse,
)


def load_instances():
    rows = []
    with open(RESULTS) as f:
        for line in f:
            try:
                d = json.loads(line)
            except Exception:
                continue
            if d.get("crashed") not in (None, "None"):
                continue
            t = d.get("time_taken")
            if t is None or t >= 295:
                continue
            text = d.get("compilable") or ""
            if not text:
                continue
            rows.append((d["instance_id"], text[:MAX_CHARS]))
    rows.sort(key=lambda r: r[0])
    return rows[:N_INSTANCES]


def run_pass(instances, clear_each):
    """clear_each=True -> cold cache per instance; False -> cache accumulates."""
    types_ts.GLOBAL_REACHABLE_CACHE.clear()
    per_instance = []
    for inst_id, text in instances:
        if clear_each:
            types_ts.GLOBAL_REACHABLE_CACHE.clear()
        state = custom_end_initial_state(END_MARKER, {})
        t0 = time.perf_counter()
        result = incremental_ts_parse(state, text)
        elapsed = time.perf_counter() - t0
        chars = len(result.parsed_code)
        ms_per_char = (elapsed * 1000.0 / chars) if chars else float("nan")
        per_instance.append((inst_id, chars, elapsed, ms_per_char, len(result)))
        print(
            f"  {inst_id[:44]:<44} chars={chars:>5} "
            f"time={elapsed:>8.3f}s  ms/char={ms_per_char:>8.3f}  states={len(result):>4}",
            flush=True,
        )
    return per_instance


def summarize(label, rows):
    vals = [r[3] for r in rows if r[3] == r[3]]
    total_time = sum(r[2] for r in rows)
    total_chars = sum(r[1] for r in rows)
    print(f"\n[{label}]")
    print(f"  instances          : {len(rows)}")
    print(f"  total chars parsed : {total_chars}")
    print(f"  total time         : {total_time:.3f} s")
    print(f"  ms/char  median    : {statistics.median(vals):.3f}")
    print(f"  ms/char  mean      : {statistics.mean(vals):.3f}")
    print(f"  ms/char  aggregate : {total_time*1000.0/total_chars:.3f}")
    print(f"  ms/char  min / max : {min(vals):.3f} / {max(vals):.3f}")


def main():
    instances = load_instances()
    print(f"selected {len(instances)} instances from {RESULTS.split('/')[-1]}")
    print(f"max chars per instance: {MAX_CHARS}\n")

    print("PASS 1: cold cache (cleared before each instance)")
    cold = run_pass(instances, clear_each=True)

    print("\nPASS 2: warm cache (cleared once, then accumulates)")
    warm = run_pass(instances, clear_each=False)

    summarize("cold", cold)
    summarize("warm", warm)


if __name__ == "__main__":
    main()
