"""Fit a per-token cost model to the PLDI paper's recorded experiment results.

Estimates, from `results_paper/*.jsonl` alone (no GPU, no model, no re-running):

    time_taken  ~  m*T  +  isConstrained * (a*T + b*K)  +  c   [+ q*T^2]

    T = generated tokens (estimated), K = total rejected candidates, isConstrained = 0/1
    m = model forward cost per token
    a = constraint cost per token
    b = constraint cost per rejected candidate
    q = optional quadratic term (gemma-2 runs set use_cache=False in sampling.py:144,
        which makes model time grow quadratically with sequence length)

Note on T: the `compilable` field is the generated program PLUS an appended test
harness that the model never generated. The test harness begins at TEST_MARKER.
Only the part before it is counted, otherwise T is inflated by ~56% and `a` is
correspondingly underestimated.

Usage:  python3 fit_cost_model.py
"""

import glob
import json
import os

RESULTS_DIR = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..",
    "type-constrained-code-generation",
    "experiments",
    "main",
    "results_paper",
)

TEST_MARKER = "declare var require"
CHARS_PER_TOKEN = 3.5  # rough; exact token counts are not stored in the results
TIMEOUT_S = 295  # records at/above this hit the 300s cap and are excluded

MODELS = [
    ("google_gemma-2-2b-it", 256000),
    ("google_gemma-2-9b-it", 256000),
    ("google_gemma-2-27b-it", 256000),
    ("Qwen_Qwen2.5-32B-Instruct", 151936),
    ("deepseek-ai_deepseek-coder-33b-instruct", 32256),
    ("codellama_CodeLlama-34b-Instruct-hf", 32016),
]


def generated_chars(text):
    """Length of the generated program, excluding the appended test harness."""
    idx = text.find(TEST_MARKER)
    return (idx if idx >= 0 else len(text)), idx >= 0


def load(model, tag):
    rows, found, total = {}, 0, 0
    pattern = os.path.join(RESULTS_DIR, f"*_{model}_s=*_t=1_synth_{tag}.jsonl")
    for path in glob.glob(pattern):
        fname = os.path.basename(path)
        for line in open(path):
            try:
                d = json.loads(line)
            except Exception:
                continue
            if d.get("crashed") not in (None, "None"):
                continue
            t = d.get("time_taken")
            if t is None or t >= TIMEOUT_S:
                continue
            text = d.get("compilable") or ""
            gen, hit = generated_chars(text)
            total += 1
            found += hit
            key = (fname.split("_")[0], d["instance_id"], fname.split("s=")[1].split("_")[0])
            rejections = sum(e[1] for e in (d.get("resamples") or []))
            rows[key] = (t, gen / CHARS_PER_TOKEN, rejections)
    return rows, found, total


def ols(X, Y):
    """Ordinary least squares via Gaussian elimination on the normal equations."""
    n = len(X[0])
    A = [
        [sum(X[r][i] * X[r][j] for r in range(len(X))) for j in range(n)]
        + [sum(X[r][i] * Y[r] for r in range(len(X)))]
        for i in range(n)
    ]
    for i in range(n):
        p = max(range(i, n), key=lambda r: abs(A[r][i]))
        A[i], A[p] = A[p], A[i]
        for r in range(n):
            if r != i and A[i][i]:
                f = A[r][i] / A[i][i]
                for c in range(i, n + 1):
                    A[r][c] -= f * A[i][c]
    return [A[i][n] / A[i][i] for i in range(n)]


def fit(model, quadratic):
    C, f1, t1 = load(model, "c")
    N, f2, t2 = load(model, "nc")
    common = set(C) & set(N)
    if len(common) < 30:
        return None
    X, Y = [], []
    for key in common:
        for rows, is_c in ((N, 0), (C, 1)):
            t, T, K = rows[key]
            row = [T, is_c * T, is_c * K, 1.0] + ([T * T] if quadratic else [])
            X.append(row)
            Y.append(t)
    sol = ols(X, Y)
    marker_rate = 100.0 * (f1 + f2) / max(t1 + t2, 1)
    return sol, len(common), marker_rate


def main():
    print(f"{'model':<42}{'vocab':>8}{'n':>6}{'m ms/tok':>10}{'a ms/tok':>10}{'b ms/rej':>10}")
    for model, vocab in MODELS:
        res = fit(model, quadratic=False)
        if res is None:
            print(f"{model:<42}{vocab:>8}     -   (too few matched instances)")
            continue
        (m, a, b, _), n, _ = res
        print(f"{model:<42}{vocab:>8}{n:>6}{m*1000:>10.1f}{a*1000:>10.1f}{b*1000:>10.3f}")

    print("\nWith quadratic model-time term (matters for gemma-2: use_cache=False):")
    for model, vocab in MODELS:
        res = fit(model, quadratic=True)
        if res is None:
            continue
        sol, n, _ = res
        m, a, b, _, q = sol
        print(
            f"  {model:<40} m={m*1000:7.2f}  a={a*1000:7.2f}  "
            f"b={b*1000:6.3f}  q={q*1e6:8.3f} us/tok^2"
        )


if __name__ == "__main__":
    main()
