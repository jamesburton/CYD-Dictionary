#!/usr/bin/env python3
"""
Audit the generated dictionary for two tier-safety gaps and emit a report for an
agent to act on:

  1. COMPOUNDS  - corpus words that contain a blocked root as a prefix/suffix
                  (e.g. "shitlist", "shitless" from "shit") and so escape the
                  exact-match category filter.
  2. LEAKS      - words VISIBLE at a tier whose definition/example text uses a
                  word that is BLOCKED at that tier (e.g. "fart" allowed at Mild
                  but its definition uses "anus", which is adult/blocked at Mild).

Inputs : tools/words_offensive.txt, words_adult.txt, words_mild.txt
         data/dict.dat, data/dict_full.idx   (run build_dict.py first)
Output : tools/tier_audit.json  (+ printed summary)
"""

import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(os.path.dirname(HERE), "data")
POS = {0: "noun", 1: "verb", 2: "adjective", 3: "adverb", 4: "other"}
WORD_RE = re.compile(r"[a-z]+")


def load_list(name):
    path = os.path.join(HERE, name)
    out = set()
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            w = line.strip().lower()
            if w and not w.startswith("#"):
                out.add(w)
    return out


def read_idx(path):
    b = open(path, "rb").read()
    assert b[:4] == b"DIDX", f"{path}: bad magic"
    _ver, count = struct.unpack("<II", b[4:12])
    terms, offs, p = [], [], 12
    for _ in range(count):
        off = struct.unpack("<I", b[p:p + 4])[0]; p += 4
        e = b.index(b"\x00", p); terms.append(b[p:e].decode("ascii")); offs.append(off); p = e + 1
    return terms, offs


def read_entry(dat, off):
    p = off
    n = dat[p]; p += 1
    ms = []
    for _ in range(n):
        pc = dat[p]; p += 1
        dl = struct.unpack("<H", dat[p:p + 2])[0]; p += 2
        d = dat[p:p + dl].decode("utf-8", "replace"); p += dl
        el = struct.unpack("<H", dat[p:p + 2])[0]; p += 2
        ex = dat[p:p + el].decode("utf-8", "replace"); p += el
        ms.append((pc, d, ex))
    return ms


def main():
    offensive = load_list("words_offensive.txt")
    adult = load_list("words_adult.txt")
    mild = load_list("words_mild.txt")
    all_blocked = offensive | adult | mild

    tier_blocked = {
        "teen": set(offensive),
        "mild": offensive | adult,
        "safe": offensive | adult | mild,
    }

    terms, offs = read_idx(os.path.join(DATA, "dict_full.idx"))
    dat = open(os.path.join(DATA, "dict.dat"), "rb").read()
    termset = set(terms)
    offmap = dict(zip(terms, offs))

    # 1. COMPOUNDS: corpus word contains a blocked root as prefix or suffix
    #    (root >= 3 chars), word itself not already a listed term.
    compounds = {}
    roots = sorted(r for r in all_blocked if len(r) >= 3)
    for w in terms:
        if w in all_blocked:
            continue
        for r in roots:
            if len(w) > len(r) and (w.startswith(r) or w.endswith(r)) and r in w:
                cat = ("offensive" if r in offensive else "adult" if r in adult else "mild")
                compounds.setdefault(r, {"category": cat, "words": []})["words"].append(w)
                break

    # 2. LEAKS: visible word whose def/example uses a word blocked at that tier
    leaks = {"teen": [], "mild": [], "safe": []}
    for w in terms:
        ms = read_entry(dat, offmap[w])
        for tier, blocked in tier_blocked.items():
            if w in blocked:
                continue  # not visible at this tier anyway
            for mi, (pc, d, ex) in enumerate(ms):
                toks = set(WORD_RE.findall((d + " " + ex).lower()))
                bad = sorted(toks & blocked)
                if bad:
                    leaks[tier].append({
                        "word": w, "tier": tier, "meaning": mi,
                        "pos": POS.get(pc, "other"),
                        "offending": bad, "def": d, "example": ex,
                    })

    report = {
        "summary": {
            "corpus_words": len(terms),
            "compound_roots_with_hits": len(compounds),
            "compound_words_total": sum(len(v["words"]) for v in compounds.values()),
            "leaks_teen": len(leaks["teen"]),
            "leaks_mild": len(leaks["mild"]),
            "leaks_safe": len(leaks["safe"]),
        },
        "compounds": {r: v for r, v in sorted(compounds.items())},
        "leaks": leaks,
    }
    out = os.path.join(HERE, "tier_audit.json")
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(report, fh, indent=1)

    s = report["summary"]
    print(json.dumps(s, indent=2))
    print(f"\nWrote {out}")
    # a few examples
    print("\nSample compounds:")
    for r, v in list(sorted(compounds.items()))[:8]:
        print(f"  {r} ({v['category']}): {', '.join(v['words'][:8])}")
    print("\nSample mild-tier leaks:")
    for L in leaks["mild"][:6]:
        print(f"  {L['word']} <- {L['offending']}: {L['def'][:60]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
