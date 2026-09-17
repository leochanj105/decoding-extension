"""Pull every LLM-generated program out of the paper's saved results into .ts files.

The paper's results live in 144 .jsonl files, one per
dataset x model x seed x task x constrained-or-not. Each line records one model
attempting one problem, mixing the generated code together with prompts, compiler
output, test results and timings.

This writes each generated program to its own .ts file, plus an index.csv holding
the metadata, so programs can be selected (by length, model, constrained-or-not)
without opening them.

    python3 extract_corpus.py

Layout:

    corpus/index.csv
    corpus/programs/<model>/<c|nc>/<dataset>_<task>_s<seed>_<instance_id>.ts

Re-runs: the paper's experiment script resumes cancelled runs by appending to the
same file, so a few problems appear more than once (e.g. one file holds 204 rows
for 159 distinct problems). Their own analysis keeps the last row for each problem
(experiments/main/analyze_avg_time.py builds a dict keyed by instance_id, so later
rows overwrite earlier ones). This follows that convention.
"""

import csv
import glob
import hashlib
import json
import os
from collections import Counter, OrderedDict

HERE = os.path.dirname(os.path.abspath(__file__))
RESULTS_DIR = os.path.join(
    HERE, "..", "type-constrained-code-generation", "experiments", "main", "results_paper"
)
OUT_DIR = os.path.join(HERE, "corpus")
PROGRAMS_DIR = os.path.join(OUT_DIR, "programs")
INDEX_FILE = os.path.join(OUT_DIR, "index.csv")

# Saved records append the benchmark's own correctness tests to the generated
# program. Those were never written by the model and the checker never saw them
# during generation, so everything from the marker on is cut.
TEST_MARKER = "declare var require"

MAX_STEM = 100  # repair-all instance ids embed a whole source filename


def parse_filename(path):
    """<dataset>_<model>_s=<seed>_t=<temp>_<task>_<c|nc>.jsonl"""
    name = os.path.basename(path)[: -len(".jsonl")]
    dataset = name.split("_")[0]
    constrained = name.split("_")[-1] == "c"
    task = name.split("_")[-2]
    seed = name.split("_s=")[1].split("_")[0]
    model = name.split("_s=")[0][len(dataset) + 1 :]
    return dataset, model, seed, task, constrained


def generated_program(record):
    """The TypeScript the model wrote, with benchmark tests stripped off."""
    text = record.get("compilable") or ""
    marker = text.find(TEST_MARKER)
    if marker >= 0:
        text = text[:marker]
    return text.rstrip()


def safe(part):
    return "".join(ch if ch.isalnum() or ch in "-._" else "_" for ch in str(part))


def make_stem(dataset, task, seed, instance):
    """Filename stem, shortened with a hash when the instance id is very long."""
    stem = safe(f"{dataset}_{task}_s{seed}_{instance}")
    if len(stem) > MAX_STEM:
        digest = hashlib.sha1(str(instance).encode()).hexdigest()[:8]
        stem = stem[: MAX_STEM - 9] + "_" + digest
    return stem


def latest_rows(path):
    """Rows of one results file, keeping only the last row per instance_id."""
    rows = OrderedDict()
    superseded = 0
    bad = 0
    for line in open(path):
        try:
            record = json.loads(line)
        except ValueError:
            bad += 1
            continue
        key = record.get("instance_id")
        if key in rows:
            superseded += 1
        rows[key] = record
    return list(rows.values()), superseded, bad


def main():
    files = sorted(glob.glob(os.path.join(RESULTS_DIR, "*.jsonl")))
    if not files:
        raise SystemExit(f"no result files found in {RESULTS_DIR}")
    os.makedirs(PROGRAMS_DIR, exist_ok=True)

    skipped = Counter()
    by_group = Counter()
    lengths = []
    seen = set()
    rows = []

    for path in files:
        dataset, model, seed, task, constrained = parse_filename(path)
        tag = "c" if constrained else "nc"
        out_dir = os.path.join(PROGRAMS_DIR, safe(model), tag)
        os.makedirs(out_dir, exist_ok=True)

        records, superseded, bad = latest_rows(path)
        skipped["superseded by a later re-run"] += superseded
        skipped["unparseable line"] += bad

        for record in records:
            if record.get("crashed") not in (None, "None"):
                skipped["generation crashed"] += 1
                continue
            if (record.get("time_taken") or 0) >= 295:
                skipped["hit 300s timeout"] += 1
                continue
            program = generated_program(record)
            if not program:
                skipped["empty program"] += 1
                continue
            if TEST_MARKER in program:
                skipped["tests not stripped"] += 1
                continue

            instance = record.get("instance_id")
            filename = make_stem(dataset, task, seed, instance) + ".ts"
            if (out_dir, filename) in seen:
                skipped["filename collision (unexpected)"] += 1
                continue
            seen.add((out_dir, filename))

            full_path = os.path.join(out_dir, filename)
            with open(full_path, "w") as handle:
                handle.write(program + "\n")

            rows.append(
                {
                    "path": os.path.relpath(full_path, OUT_DIR),
                    "chars": len(program),
                    "lines": program.count("\n") + 1,
                    "instance_id": instance,
                    "dataset": dataset,
                    "model": model,
                    "seed": seed,
                    "task": task,
                    "constrained": int(constrained),
                    "tests_passed": int(bool(record.get("tests_passed"))),
                    "time_taken": record.get("time_taken"),
                }
            )
            lengths.append(len(program))
            by_group[(model, tag)] += 1

    rows.sort(key=lambda r: r["path"])
    with open(INDEX_FILE, "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)

    lengths.sort()
    print(f"read {len(files)} result files")
    print(f"wrote {len(rows)} .ts files -> {os.path.relpath(PROGRAMS_DIR, HERE)}/")
    print(f"index               -> {os.path.relpath(INDEX_FILE, HERE)}")
    print(f"total {sum(lengths)/1e6:.1f} MB of TypeScript\n")

    print("skipped:")
    for reason, count in skipped.most_common():
        if count:
            print(f"  {count:>7}  {reason}")

    print("\nprogram length in characters:")
    for label, idx in (
        ("min", 0),
        ("25%", len(lengths) // 4),
        ("median", len(lengths) // 2),
        ("75%", 3 * len(lengths) // 4),
        ("95%", int(0.95 * len(lengths))),
        ("max", len(lengths) - 1),
    ):
        print(f"  {label:>6}: {lengths[idx]}")

    print("\nprograms per model (c = type-constrained, nc = not):")
    for model in sorted({m for m, _ in by_group}):
        print(
            f"  {model:<42} c={by_group[(model, 'c')]:>6}  nc={by_group[(model, 'nc')]:>6}"
        )


if __name__ == "__main__":
    main()
