# The function body (from the "{" after "-> Self " to its matching "}") of the saved
# Qwen2.5-Coder-1.5B greedy BitWriter::new output, byte for byte. The output has no
# strings, characters or comments, so counting braces finds the matching "}".
# Usage: extract_bitwriter_body.py OUTPUTS.jsonl OUT.rs   (write the fixture)
#        extract_bitwriter_body.py OUTPUTS.jsonl --check FIXTURE.rs
import json
import sys

path, *rest = sys.argv[1:]
(record,) = [r for r in map(json.loads, open(path))
             if r["task"] == "bitwriter_new" and r["model"] == "qwen2.5-coder-1.5b-q8_0"
             and r["mode"] == "greedy"]
output = record["output"]
assert output.count("-> Self {") == 1
start = output.index("-> Self {") + len("-> Self ")
depth = 0
for end in range(start, len(output)):
    depth += {"{": 1, "}": -1}.get(output[end], 0)
    if depth == 0:
        break
assert depth == 0
body = output[start:end + 1]

if rest[0] == "--check":
    ok = open(rest[1], newline="").read() == body
    print(("same" if ok else "DIFFERENT") + f": {rest[1]} vs the record in {path}")
    sys.exit(0 if ok else 1)
with open(rest[0], "w", newline="") as f:
    f.write(body)
