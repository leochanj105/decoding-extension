"""How big is the per-type identifier token mask, really?

Replays a program through TCD, and at every decoding step takes the live symbol
environment, groups the names by type, and counts the vocabulary tokens that can
legally START one of those names. That count is the size of the tier-1 mask the
design needs, and it decides sparse-vs-dense representation.

A token can start a name iff the token's text is a prefix of the name (the full
name included). So per name it is enough to look up each of its prefixes in the
vocabulary -- no 151k scan.
"""
import argparse, csv, os, sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.join(HERE, "..", "type-constrained-code-generation")
sys.path.insert(0, REPO)
CORPUS_DIR = os.path.join(HERE, "corpus")
END_MARKER = "```"

from typesafe_llm.parser.parser_ts import custom_end_initial_state, incremental_ts_parse


def bytes_to_unicode():
    bs = list(range(ord("!"), ord("~") + 1)) + list(range(0xA1, 0xAD)) + list(range(0xAE, 0x100))
    cs = bs[:]
    n = 0
    for b in range(256):
        if b not in bs:
            bs.append(b); cs.append(256 + n); n += 1
    return dict(zip(bs, [chr(c) for c in cs]))


def decoded_vocab(tokenizer_json):
    from tokenizers import Tokenizer
    tk = Tokenizer.from_file(tokenizer_json)
    b2u = bytes_to_unicode()
    u2b = {v: k for k, v in b2u.items()}
    out = {}
    for enc, tid in tk.get_vocab().items():
        try:
            raw = bytes(u2b[ch] for ch in enc)
        except KeyError:
            continue                      # special / added token
        try:
            out[raw.decode("utf-8")] = tid
        except UnicodeDecodeError:
            continue
    return tk, out


def _all_nodes(state):
    yield state
    active = getattr(state, "active", None)
    if isinstance(active, (list, tuple)):
        kids = [c for c in active if hasattr(c, "identifiers")]
    elif hasattr(active, "identifiers"):
        kids = [active]
    else:
        kids = []
    for kid in kids:
        yield from _all_nodes(kid)


def live_symbols(parser_state):
    symbols = {}
    for top in parser_state.active_states:
        for node in _all_nodes(top):
            for name, entry in getattr(node, "identifiers", {}).items():
                typ = entry[0] if isinstance(entry, tuple) else entry
                symbols[name] = str(typ)
    return symbols


def pick_program(results, min_states):
    best = None
    for row in csv.DictReader(open(os.path.join(HERE, results))):
        if row["status"] != "ok" or not row["states"]:
            continue
        if int(row["states"]) < min_states:
            continue
        if best is None or float(row["seconds"]) > float(best["seconds"]):
            best = row
    if not best:
        raise SystemExit("no program matched")
    return os.path.join(CORPUS_DIR, best["path"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("program", nargs="?")
    ap.add_argument("--states", type=int, default=1001)
    ap.add_argument("--results", default="results_quick.csv")
    ap.add_argument("--tokenizer", default=os.path.join(HERE, "tok.json"))
    ap.add_argument("--out", default="symbol_masks")
    args = ap.parse_args()

    path = args.program or pick_program(args.results, args.states)
    text = open(path).read().rstrip("\n")
    tk, vocab = decoded_vocab(args.tokenizer)
    print(f"{os.path.basename(path)}: {len(text)} chars; decoded vocab {len(vocab):,}")

    ids = tk.encode(text).ids
    pieces = [p for p in (tk.decode([i]) for i in ids) if p]

    # tokens that can start a given name == its prefixes present in the vocabulary
    bare_cache, spaced_cache = {}, {}

    def mask_bare(name):
        if name not in bare_cache:
            bare_cache[name] = {vocab[name[:i]] for i in range(1, len(name) + 1)
                                if name[:i] in vocab}
        return bare_cache[name]

    def mask_spaced(name):
        if name not in spaced_cache:
            spaced_cache[name] = {vocab[" " + name[:i]] for i in range(1, len(name) + 1)
                                  if " " + name[:i] in vocab}
        return spaced_cache[name]

    state = custom_end_initial_state(END_MARKER, {})
    rows, per_type_sizes = [], []
    for step, piece in enumerate(pieces):
        state = incremental_ts_parse(state, piece)
        if not state.active_states:
            print(f"rejected at step {step} {piece!r}")
            break
        syms = live_symbols(state)
        by_type = defaultdict(set)
        for n, t in syms.items():
            by_type[t].add(n)

        type_masks = {}
        for t, names in by_type.items():
            m = set()
            for n in names:
                m |= mask_bare(n)
            type_masks[t] = m
            per_type_sizes.append(len(m))
        union = set().union(*type_masks.values()) if type_masks else set()
        union_sp = set()
        for n in syms:
            union_sp |= mask_spaced(n)

        sizes = sorted((len(m) for m in type_masks.values()), reverse=True)
        rows.append({
            "step": step, "live_symbols": len(syms), "types": len(by_type),
            "union_tokens": len(union),
            "union_tokens_with_space_variant": len(union | union_sp),
            "max_per_type": sizes[0] if sizes else 0,
            "median_per_type": sizes[len(sizes) // 2] if sizes else 0,
            "types_over_1000": sum(1 for s in sizes if s > 1000),
        })

    out = os.path.join(HERE, args.out + "_steps.csv")
    with open(out, "w", newline="") as h:
        w = csv.DictWriter(h, fieldnames=list(rows[0].keys())); w.writeheader(); w.writerows(rows)

    u = [r["union_tokens"] for r in rows]
    us = [r["union_tokens_with_space_variant"] for r in rows]
    mx = [r["max_per_type"] for r in rows]
    def q(xs, p):
        xs = sorted(xs); return xs[min(len(xs) - 1, int(p * len(xs)))]
    print(f"\nsteps replayed            : {len(rows)}")
    print(f"live symbols              : {min(r['live_symbols'] for r in rows)}"
          f"-{max(r['live_symbols'] for r in rows)}")
    print(f"distinct types            : {min(r['types'] for r in rows)}"
          f"-{max(r['types'] for r in rows)}")
    print(f"union mask (bare)         : median {q(u,.5)}  p90 {q(u,.9)}  max {max(u)}")
    print(f"union mask (+space forms) : median {q(us,.5)}  p90 {q(us,.9)}  max {max(us)}")
    print(f"largest single-type mask  : median {q(mx,.5)}  max {max(mx)}")
    print(f"per-type mask sizes       : median {q(per_type_sizes,.5)}"
          f"  p99 {q(per_type_sizes,.99)}  max {max(per_type_sizes)}")
    print(f"type-masks over 1000      : {sum(1 for s in per_type_sizes if s > 1000)}"
          f" of {len(per_type_sizes)}")
    print(f"dense bitmask would be    : {(len(vocab)+31)//32*4:,} bytes per mask")
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
