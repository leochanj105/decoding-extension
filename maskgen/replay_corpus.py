"""Walk corpus programs token by token: accept the legal ones, reject the illegal.

profiling/profile_advance.py feeds a whole program at once and only looks at the
final state, so it can say a program was rejected but not where. This feeds one
token at a time and records the first token the checker refuses.

    ./venv/bin/python ../maskgen/replay_corpus.py --kind c --limit 200 --jobs 8

Results append to a CSV, so a run can be interrupted and resumed.

A token counts as accepted only if the checker both survives it and consumes all
of its characters: a surviving state that stopped partway through the token means
the tail was refused.

Note that `nc` programs were generated without type checking but are frequently
well-typed anyway, so a rejection is a population tendency, not a per-program
guarantee.

Needs Python 3.11 and: frozenlist regex termcolor frozendict tokenizers
"""

from __future__ import annotations

import argparse
import collections
import csv
import os
import signal
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.join(HERE, "..", "type-constrained-code-generation")
if REPO not in sys.path:
    sys.path.insert(0, REPO)
CORPUS_DIR = os.path.join(HERE, "..", "profiling", "corpus")
INDEX_FILE = os.path.join(CORPUS_DIR, "index.csv")
TOKENIZER = os.path.join(HERE, "..", "profiling", "tok.json")

# The checker was run with this end-of-code marker during generation.
END_MARKER = "```"

FIELDS = [
    "path", "kind", "model", "chars", "tokens", "tokens_accepted",
    "first_rejected_step", "first_rejected_token", "status", "seconds",
]

_tokenizer = None


def tokenizer():
    global _tokenizer
    if _tokenizer is None:
        from tokenizers import Tokenizer

        _tokenizer = Tokenizer.from_file(TOKENIZER)
    return _tokenizer


class Timeout(Exception):
    pass


def _alarm(signum, frame):
    raise Timeout()


def token_pieces(text: str) -> list[str]:
    tok = tokenizer()
    pieces = [tok.decode([i]) for i in tok.encode(text).ids]
    # A piece that decodes to nothing cannot be fed to the checker and would make
    # the step indices disagree with the model's own, so drop it.
    return [p for p in pieces if p]


def replay_one(path: str, kind: str, model: str, timeout: float) -> dict:
    from typesafe_llm.parser.parser_ts import (
        custom_end_initial_state,
        incremental_ts_parse,
    )

    # extract_corpus.py ends each file with a newline the model never generated.
    # The checker applies automatic semicolon insertion at a newline, which can
    # reject a program that was merely unfinished, so drop it.
    text = open(os.path.join(CORPUS_DIR, path)).read().rstrip("\n")
    pieces = token_pieces(text)

    state = custom_end_initial_state(END_MARKER, {})
    consumed = 0
    accepted = 0
    first_step, first_token, status = "", "", "all_accepted"

    signal.signal(signal.SIGALRM, _alarm)
    signal.setitimer(signal.ITIMER_REAL, timeout)
    start = time.perf_counter()
    try:
        for step, piece in enumerate(pieces):
            nxt = incremental_ts_parse(state, piece)
            if not nxt.active_states:
                first_step, first_token, status = step, repr(piece), "rejected"
                break
            if len(nxt.parsed_code) < consumed + len(piece):
                # survived, but refused part of this token
                first_step, first_token, status = step, repr(piece), "partial"
                break
            consumed += len(piece)
            accepted += 1
            state = nxt
        seconds = time.perf_counter() - start
    except Timeout:
        seconds = time.perf_counter() - start
        status = "timeout"
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)

    return {
        "path": path, "kind": kind, "model": model,
        "chars": len(text), "tokens": len(pieces), "tokens_accepted": accepted,
        "first_rejected_step": first_step, "first_rejected_token": first_token,
        "status": status, "seconds": round(seconds, 4),
    }


def _worker(args):
    return replay_one(*args)


def select(args) -> list[tuple[str, str, str]]:
    rows = list(csv.DictReader(open(INDEX_FILE)))
    out = []
    for r in rows:
        kind = r["path"].split("/")[2]
        if args.kind != "both" and kind != args.kind:
            continue
        if args.model and args.model not in r["model"]:
            continue
        chars = int(r["chars"])
        if chars < args.min_chars or (args.max_chars and chars > args.max_chars):
            continue
        out.append((r["path"], kind, r["model"]))
    out.sort(key=lambda t: t[0])
    if args.limit:
        # Apply the limit per kind. Paths group all of a model's c/ before its nc/,
        # so a flat cut would silently return only one kind.
        per_kind: dict[str, list] = {}
        for item in out:
            per_kind.setdefault(item[1], []).append(item)
        out = [x for kind in sorted(per_kind) for x in per_kind[kind][: args.limit]]
    return out


def already_done(out_path: str) -> set[str]:
    if not os.path.exists(out_path):
        return set()
    with open(out_path) as handle:
        return {r["path"] for r in csv.DictReader(handle)}


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--kind", choices=["c", "nc", "both"], default="both")
    ap.add_argument("--model", default="", help="substring of the model name")
    ap.add_argument("--limit", type=int, default=0, help="0 = no limit")
    ap.add_argument("--min-chars", type=int, default=1)
    ap.add_argument("--max-chars", type=int, default=0, help="0 = no limit")
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--jobs", type=int, default=1)
    ap.add_argument("--out", default=os.path.join(HERE, "replay_corpus.csv"))
    args = ap.parse_args()

    chosen = select(args)
    done = already_done(args.out)
    todo = [c for c in chosen if c[0] not in done]
    print(f"{len(chosen)} selected, {len(done)} already recorded, {len(todo)} to run")
    print(f"timeout {args.timeout}s per program, {args.jobs} job(s)")

    fresh = not os.path.exists(args.out)
    handle = open(args.out, "a", newline="")
    writer = csv.DictWriter(handle, fieldnames=FIELDS)
    if fresh:
        writer.writeheader()

    results = []
    work = [(p, k, m, args.timeout) for p, k, m in todo]
    started = time.perf_counter()
    try:
        if args.jobs > 1:
            import multiprocessing

            with multiprocessing.Pool(args.jobs) as pool:
                for i, row in enumerate(pool.imap_unordered(_worker, work), 1):
                    writer.writerow(row); handle.flush(); results.append(row)
                    if i % 50 == 0:
                        print(f"  {i}/{len(work)}  {time.perf_counter()-started:.0f}s")
        else:
            for i, item in enumerate(work, 1):
                row = _worker(item)
                writer.writerow(row); handle.flush(); results.append(row)
                if i % 50 == 0:
                    print(f"  {i}/{len(work)}  {time.perf_counter()-started:.0f}s")
    finally:
        handle.close()

    if not results:
        return
    print(f"\nran {len(results)} in {time.perf_counter()-started:.0f}s -> {args.out}\n")
    for kind in ("c", "nc"):
        subset = [r for r in results if r["kind"] == kind]
        if not subset:
            continue
        tally = collections.Counter(r["status"] for r in subset)
        print(f"{kind}: {len(subset)} programs")
        for status, count in tally.most_common():
            print(f"    {status:14} {count:6}  ({100*count/len(subset):.1f}%)")
        refused = [r for r in subset if r["status"] in ("rejected", "partial")]
        if refused:
            frac = [r["tokens_accepted"] / r["tokens"] for r in refused if r["tokens"]]
            frac.sort()
            print(f"    of those refused, share of tokens accepted first: "
                  f"median {frac[len(frac)//2]:.0%}")


if __name__ == "__main__":
    main()
