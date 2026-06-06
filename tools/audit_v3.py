#!/usr/bin/env python3
"""
Simulate the format-v3 split rule on the revised lists and report the gate numbers:
  - per-tier visible word counts
  - override residue: words pushed ABOVE their headword floor by a core-harmful word
    in their gloss (these need a sanitised override to stay visible, else they hide)
  - over-hide risk: of those, words with a CLEAN headword now hidden at Safe
Reads tools/{words_*.txt, core_harmful.txt} and data/{dict.dat, dict_full.idx}.
"""
import os, re, struct, collections

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(os.path.dirname(HERE), "data")
WORD_RE = re.compile(r"[a-z]+")
SAFE, MILD, TEEN, FULL = 0, 1, 2, 3
NAME = {0: "Safe", 1: "Mild", 2: "Teen", 3: "Full"}


def load(name):
    p = os.path.join(HERE, name); s = set()
    if os.path.exists(p):
        for ln in open(p, encoding="utf-8"):
            w = ln.strip().lower()
            if w and not w.startswith("#"): s.add(w)
    return s


def read_idx(path):
    b = open(path, "rb").read(); _v, c = struct.unpack("<II", b[4:12])
    terms, offs, p = [], [], 12
    for _ in range(c):
        off = struct.unpack("<I", b[p:p+4])[0]; p += 4
        e = b.index(b"\x00", p); terms.append(b[p:e].decode()); offs.append(off); p = e+1
    return terms, offs


def read_entry(dat, off):
    p = off; n = dat[p]; p += 1; ms = []
    for _ in range(n):
        pc = dat[p]; p += 1
        dl = struct.unpack("<H", dat[p:p+2])[0]; p += 2; d = dat[p:p+dl].decode("utf-8","replace"); p += dl
        el = struct.unpack("<H", dat[p:p+2])[0]; p += 2; ex = dat[p:p+el].decode("utf-8","replace"); p += el
        ms.append((pc, d, ex))
    return ms


def main():
    offensive, adult, mild = load("words_offensive.txt"), load("words_adult.txt"), load("words_mild.txt")
    core = load("core_harmful.txt")

    def hw_floor(w):
        return FULL if w in offensive else TEEN if w in adult else MILD if w in mild else SAFE

    def core_floor(text):
        fl = SAFE
        for t in set(WORD_RE.findall(text.lower())) & core:
            fl = max(fl, FULL if t in offensive else TEEN)
        return fl

    terms, offs = read_idx(os.path.join(DATA, "dict_full.idx"))
    dat = open(os.path.join(DATA, "dict.dat"), "rb").read()

    visible = collections.Counter()
    pushed = []          # (word, hw, wmin) where core vocab raised the floor
    overhide = []        # clean headword now hidden at Safe
    for w, off in zip(terms, offs):
        hw = hw_floor(w)
        mts = [max(hw, core_floor(d + " " + ex)) for (_pc, d, ex) in read_entry(dat, off)]
        wmin = min(mts)
        for T in (SAFE, MILD, TEEN, FULL):
            if wmin <= T: visible[T] += 1
        if wmin > hw:
            pushed.append((w, hw, wmin))
            if hw == SAFE and wmin > SAFE:
                overhide.append((w, wmin))

    print("Visible word counts per tier:")
    for T in (SAFE, MILD, TEEN, FULL):
        print(f"  {NAME[T]:5}: {visible[T]}")
    print(f"\nOverride residue (gloss core-word pushed word above its headword floor): {len(pushed)}")
    bt = collections.Counter(NAME[wmin] for _, _, wmin in pushed)
    print("  by raised-to tier:", dict(bt))
    print(f"\nOVER-HIDE RISK (clean headword now hidden at Safe): {len(overhide)}")
    for w, wmin in overhide[:40]:
        print(f"    {w}  -> {NAME[wmin]}")
    if len(overhide) > 40:
        print(f"    ... +{len(overhide)-40} more")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
