# Ownership pilot: for each saved prompt, one greedy completion and four sampled ones
# (temperature 0.7, seeds 1-4) from a running llama-server; appends one JSON line per
# completion. Generation only: outputs are stored exactly as returned.
# Usage: pilot_generate.py SERVER_URL MODEL_ID MODEL_REPO MODEL_REVISION MODEL_FILE OUT.jsonl PROMPT...
import json
import os
import sys
import time
import urllib.request

# Every llama.cpp sampler other than temperature / seed set to its neutral value.
NEUTRAL = {
    "n_predict": 512,
    "top_k": 0,  # disabled
    "top_p": 1.0,
    "min_p": 0.0,
    "typical_p": 1.0,
    "xtc_probability": 0.0,
    "dry_multiplier": 0.0,
    "mirostat": 0,
    "repeat_penalty": 1.0,
    "presence_penalty": 0.0,
    "frequency_penalty": 0.0,
    "cache_prompt": False,
}
RUNS = [("greedy", 0.0, 0)] + [("sampled", 0.7, seed) for seed in (1, 2, 3, 4)]


def post(url, body):
    req = urllib.request.Request(url, json.dumps(body).encode(), {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=600) as r:
        return json.load(r)


server, model_id, repo, revision, model_file, out_path, *prompts = sys.argv[1:]
with open(out_path, "a") as out:
    for prompt_path in prompts:
        task = os.path.basename(prompt_path).removesuffix(".prompt.txt")
        with open(prompt_path) as f:
            prompt = f.read()
        messages = [{"role": "user", "content": prompt}]
        formatted = post(server + "/apply-template", {"messages": messages})["prompt"]
        for mode, temperature, seed in RUNS:
            settings = {**NEUTRAL, "temperature": temperature, "seed": seed}
            start = time.monotonic()
            response = post(server + "/completion", {"prompt": formatted, **settings})
            wall_s = time.monotonic() - start
            record = {
                "task": task, "prompt_file": os.path.basename(prompt_path),
                "model": model_id, "model_repo": repo, "model_revision": revision, "model_file": model_file,
                "mode": mode, "seed": seed if mode == "sampled" else None,
                "settings_sent": settings, "formatted_prompt": formatted,
                "output": response["content"],
                "tokens_prompt": response["tokens_evaluated"], "tokens_output": response["tokens_predicted"],
                "stop_type": response["stop_type"], "wall_s": wall_s,
                "timings": response["timings"], "generation_settings": response["generation_settings"],
            }
            out.write(json.dumps(record) + "\n")
            out.flush()
            print(f"{task} {model_id} {mode} seed={record['seed']}: {response['tokens_predicted']} tokens, "
                  f"stop {response['stop_type']}, {wall_s:.1f} s", flush=True)
