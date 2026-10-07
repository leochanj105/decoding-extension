"""Do real tokens cross identifier boundaries?

A tier-1 mask lists tokens that are a PREFIX of some in-scope name. A token that
finishes a name and keeps going (`s);`) is not such a prefix, so a plain
intersection with a syntactic mask would block it. This measures how often the
tokens a model actually emitted do that.

Purely lexical: tokenize each program, take character offsets, find identifier
spans with a regex, and see whether the token covering a span's last character
extends past it (trailing straddle) or the token covering its first character
starts before it (leading straddle).

    ./venv/bin/python measure_straddle.py --programs 800
"""
import argparse, csv, os, random, re, sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, "corpus")
IDENT = re.compile(r"[A-Za-z_$][A-Za-z0-9_$]*")
KEYWORDS = {
    "const","let","var","function","return","if","else","for","while","do","break",
    "continue","new","typeof","instanceof","in","of","class","extends","this","super",
    "true","false","null","undefined","void","delete","try","catch","finally","throw",
    "switch","case","default","yield","async","await","import","export","from","as",
    "interface","type","enum","implements","private","public","protected","readonly",
    "static","declare","namespace","module","abstract","is","keyof","infer","never",
    "unknown","any","number","string","boolean","object","symbol","bigint",
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--programs", type=int, default=800)
    ap.add_argument("--tokenizer", default=os.path.join(HERE, "tok.json"))
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()

    from tokenizers import Tokenizer
    tok = Tokenizer.from_file(args.tokenizer)

    rows = [r for r in csv.DictReader(open(os.path.join(CORPUS, "index.csv")))
            if r.get("constrained", "") in ("c", "True", "true", "1")
            or "/c/" in r.get("path", "")]
    if not rows:
        rows = list(csv.DictReader(open(os.path.join(CORPUS, "index.csv"))))
    random.Random(args.seed).shuffle(rows)

    tot = Counter()
    overflow_text = Counter()
    underflow_text = Counter()
    per_prog = []
    used = 0

    for r in rows:
        if used >= args.programs:
            break
        path = os.path.join(CORPUS, r["path"])
        try:
            text = open(path).read()
        except OSError:
            continue
        if not text.strip():
            continue
        enc = tok.encode(text)
        offs = enc.offsets
        if not offs:
            continue
        used += 1

        # char index -> index of the token covering it
        owner = [-1] * (len(text) + 1)
        for i, (a, b) in enumerate(offs):
            for c in range(a, min(b, len(text))):
                owner[c] = i

        p_ident = p_trail = p_lead = 0
        for m in IDENT.finditer(text):
            name = m.group(0)
            if name in KEYWORDS:
                continue
            s, e = m.start(), m.end()
            ti, tl = owner[e - 1], owner[s]
            if ti < 0 or tl < 0:
                continue
            tot["identifiers"] += 1
            p_ident += 1
            if offs[ti][1] > e:                       # token runs past the name
                tot["trailing_straddle"] += 1
                p_trail += 1
                overflow_text[text[e:offs[ti][1]][:4]] += 1
            if offs[tl][0] < s:                       # token began before the name
                tot["leading_straddle"] += 1
                p_lead += 1
                underflow_text[text[offs[tl][0]:s][-4:]] += 1
            if offs[ti][1] > e or offs[tl][0] < s:
                tot["either"] += 1
            # tokens wholly inside the name
            tot["tokens_in_name"] += len({owner[c] for c in range(s, e)})
        per_prog.append((p_ident, p_trail, p_lead))

    n = tot["identifiers"]
    print(f"programs           : {used}")
    print(f"identifier uses    : {n:,}  (keywords excluded)")
    print(f"tokens inside names: {tot['tokens_in_name']:,}"
          f"  ({tot['tokens_in_name']/n:.2f} per identifier)")
    print()
    print(f"trailing straddle  : {tot['trailing_straddle']:,}"
          f"  ({100*tot['trailing_straddle']/n:.1f}%)   <- tier-1 alone would block these")
    print(f"leading straddle   : {tot['leading_straddle']:,}"
          f"  ({100*tot['leading_straddle']/n:.1f}%)")
    print(f"either             : {tot['either']:,}  ({100*tot['either']/n:.1f}%)")
    print()
    print("what the trailing overflow contains (top 15):")
    for t, c in overflow_text.most_common(15):
        print(f"   {t!r:12} {c:7,}  {100*c/max(1,tot['trailing_straddle']):5.1f}%")
    print()
    print("what precedes, on leading straddle (top 10):")
    for t, c in underflow_text.most_common(10):
        print(f"   {t!r:12} {c:7,}  {100*c/max(1,tot['leading_straddle']):5.1f}%")

    fr = sorted(100 * t / i for i, t, _ in per_prog if i)
    if fr:
        print()
        print(f"per-program trailing-straddle rate: median {fr[len(fr)//2]:.1f}%"
              f"  p10 {fr[len(fr)//10]:.1f}%  p90 {fr[9*len(fr)//10]:.1f}%")


if __name__ == "__main__":
    main()
