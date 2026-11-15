# Render outputs.jsonl as outputs.md: every raw output, labelled, in a fence long
# enough that backticks inside the output cannot close it.
# Usage: pilot_outputs_md.py outputs.jsonl outputs.md
import json
import sys

records = [json.loads(line) for line in open(sys.argv[1])]
lines = ["# Ownership pilot: raw outputs", "",
         "Each output is shown exactly as generated, inside a `~~~~~~~~` fence (nothing is from the model outside it).", ""]
for r in records:
    seed = "-" if r["seed"] is None else r["seed"]
    lines += [f"## {r['task']} | {r['model']} | {r['mode']} | seed {seed}", "",
              f"{r['tokens_output']} output tokens, stop: {r['stop_type']}, "
              f"generation {r['timings']['predicted_ms']:.0f} ms, wall {r['wall_s']:.1f} s", "",
              "~~~~~~~~", r["output"], "~~~~~~~~", ""]
with open(sys.argv[2], "w") as f:
    f.write("\n".join(lines))
