# One greedy, unconstrained completion per task from a running llama-server (which
# keeps the model loaded across requests). Usage: generate.py TASKS.jsonl SERVER_URL OUT.jsonl
#
# The user message is INSTRUCTION, a blank line, then the original prompt. The server
# applies the model's own chat template (/apply-template); that exact text is saved and
# sent to /completion. Every response is saved whole, alongside a client-side wall time.
import json
import sys
import time
import urllib.request

INSTRUCTION = ("Complete this Rust function. Return only its body, without the outer braces, "
               "Markdown, or explanation.")

SETTINGS = {
    "n_predict": 512,
    "temperature": 0.0,  # greedy
    "top_k": 1,
    "repeat_penalty": 1.0,  # 1.0 / 0.0 = repetition penalties disabled
    "presence_penalty": 0.0,
    "frequency_penalty": 0.0,
    "dry_multiplier": 0.0,
    "seed": 0,
    "cache_prompt": False,  # each request evaluates its whole prompt
}


def post(url, body):
    req = urllib.request.Request(url, json.dumps(body).encode(), {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=600) as r:
        return json.load(r)


tasks_path, server, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
with open(tasks_path) as f, open(out_path, "w") as out:
    for task in map(json.loads, f):
        messages = [{"role": "user", "content": INSTRUCTION + "\n\n" + task["prompt"]}]
        formatted = post(server + "/apply-template", {"messages": messages})["prompt"]
        start = time.monotonic()
        response = post(server + "/completion", {"prompt": formatted, **SETTINGS})
        wall_s = time.monotonic() - start
        record = {"name": task["name"], "messages": messages, "formatted_prompt": formatted,
                  "settings": SETTINGS, "wall_s": wall_s, "response": response}
        out.write(json.dumps(record) + "\n")
        print(f"{task['name']}: {response['tokens_predicted']} tokens, stop {response['stop_type']}, "
              f"{wall_s:.1f} s", flush=True)
