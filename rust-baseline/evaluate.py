# Build each program as original prompt + generated body + original tests (exact
# concatenation, no repair), compile it with rustc --edition 2021 (30 s limit), and run
# it (2 s limit). Usage: evaluate.py TASKS.jsonl GENERATIONS.jsonl OUT_DIR
#
# Outcome: passed | compilation failed | execution failed | timed out (compile or run).
# Generation truncation (stop_type "limit") is reported separately.
import json
import os
import subprocess
import sys

tasks_path, gens_path, out_dir = sys.argv[1], sys.argv[2], sys.argv[3]
tasks = {t["name"]: t for t in map(json.loads, open(tasks_path))}


def run(cmd, timeout):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return {"cmd": cmd, "returncode": p.returncode, "stdout": p.stdout, "stderr": p.stderr, "timed_out": False}
    except subprocess.TimeoutExpired as e:
        return {"cmd": cmd, "returncode": None, "stdout": e.stdout or "", "stderr": e.stderr or "", "timed_out": True}


results = []
for gen in map(json.loads, open(gens_path)):
    task, resp = tasks[gen["name"]], gen["response"]
    d = os.path.join(out_dir, gen["name"])
    os.makedirs(d, exist_ok=True)
    src, exe = os.path.join(d, "main.rs"), os.path.join(d, "main")
    with open(src, "w") as f:
        f.write(task["prompt"] + resp["content"] + task["tests"])

    compile_ = run(["rustc", "--edition", "2021", "-o", exe, src], 30)
    execute = None
    if compile_["timed_out"]:
        outcome = "timed out (compile)"
    elif compile_["returncode"] != 0:
        outcome = "compilation failed"
    else:
        execute = run([exe], 2)
        outcome = ("timed out (run)" if execute["timed_out"] else
                   "passed" if execute["returncode"] == 0 else "execution failed")

    r = {"name": gen["name"], "outcome": outcome, "truncated": resp["stop_type"] == "limit",
         "stop_type": resp["stop_type"], "tokens_predicted": resp["tokens_predicted"],
         "tokens_prompt": resp["tokens_evaluated"], "generation_ms": resp["timings"]["predicted_ms"],
         "prompt_ms": resp["timings"]["prompt_ms"], "wall_s": gen["wall_s"],
         "compile": compile_, "run": execute}
    with open(os.path.join(d, "result.json"), "w") as f:
        json.dump(r, f, indent=1)
    results.append(r)

with open(os.path.join(out_dir, "summary.md"), "w") as f:
    f.write("| task | gen tokens | stop | truncated | prompt ms | gen ms | wall s | outcome |\n")
    f.write("|---|---:|---|---|---:|---:|---:|---|\n")
    for r in results:
        f.write(f"| {r['name']} | {r['tokens_predicted']} | {r['stop_type']} | {'YES' if r['truncated'] else 'no'} "
                f"| {r['prompt_ms']:.0f} | {r['generation_ms']:.0f} | {r['wall_s']:.1f} | {r['outcome']} |\n")
print(open(os.path.join(out_dir, "summary.md")).read())
