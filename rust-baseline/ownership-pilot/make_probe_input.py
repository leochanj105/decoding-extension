# Prefix probe input: the stored chat-templated prompt of one outputs.jsonl record,
# followed by that record's raw output cut right after MARKER (which must occur
# exactly once); no end marker is added, so the assistant turn is left unfinished.
# Usage: make_probe_input.py outputs.jsonl TASK MODEL MODE MARKER OUT.txt
import json
import sys

path, task, model, mode, marker, out_path = sys.argv[1:]
(record,) = [r for r in map(json.loads, open(path))
             if r["task"] == task and r["model"] == model and r["mode"] == mode]
output = record["output"]
assert output.count(marker) == 1
cut = output.index(marker) + len(marker)
with open(out_path, "w", newline="") as f:
    f.write(record["formatted_prompt"] + output[:cut])
