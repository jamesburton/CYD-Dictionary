#!/usr/bin/env python3
"""Parse the generated data/ files and assert correctness. Exit non-zero on failure."""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(os.path.dirname(HERE), "data")
POS = {0: "noun", 1: "verb", 2: "adjective", 3: "adverb", 4: "other"}

IDX_FILES = {
    "full": "dict_full.idx",
    "teen": "dict_teen.idx",
    "mild": "dict_mild.idx",
    "safe": "dict_safe.idx",
}


def load_category(name):
    """Load a word category list from tools/ (one per line, # comments skipped)."""
    path = os.path.join(HERE, name)
    out = []
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as fh:
            for line in fh:
                w = line.strip().lower()
                if w and not w.startswith("#"):
                    out.append(w)
    return out


def read_idx(path):
    b = open(path, "rb").read()
    assert b[:4] == b"DIDX", f"{path}: bad magic"
    ver, count = struct.unpack("<II", b[4:12])
    assert ver == 2, f"{path}: version {ver} != 2"
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
    # ── Load all four indexes ──────────────────────────────────────────────────
    tiers = {}
    for tier, fname in IDX_FILES.items():
        path = os.path.join(DATA, fname)
        terms, offs = read_idx(path)
        tiers[tier] = {"terms": terms, "offs": offs, "set": set(terms), "map": dict(zip(terms, offs))}
        assert terms == sorted(terms), f"{fname} is not sorted"

    dat = open(os.path.join(DATA, "dict.dat"), "rb").read()

    full_map = tiers["full"]["map"]

    # ── Subset chain: safe ⊆ mild ⊆ teen ⊆ full ──────────────────────────────
    assert tiers["safe"]["set"] <= tiers["mild"]["set"], "safe is not a subset of mild"
    assert tiers["mild"]["set"] <= tiers["teen"]["set"], "mild is not a subset of teen"
    assert tiers["teen"]["set"] <= tiers["full"]["set"], "teen is not a subset of full"

    # ── Shared offsets: every smaller-tier word uses the same offset as full ──
    for tier_name, tier in tiers.items():
        if tier_name == "full":
            continue
        for t, o in tier["map"].items():
            assert full_map[t] == o, f"{tier_name} offset mismatch for '{t}'"

    # ── Primary-sense sanity for blue / toy / mouse ────────────────────────────
    for w in ("blue", "toy", "mouse"):
        assert w in tiers["full"]["set"], f"'{w}' missing from full"
        pc, d, _ = read_entry(dat, full_map[w])[0]
        print(f"{w}: [{POS[pc]}] {d[:60]}")

    # blue must mention colour/color somewhere across its senses
    blue_defs = " ".join(d for _, d, _ in read_entry(dat, full_map["blue"])).lower()
    assert "colour" in blue_defs or "color" in blue_defs, "blue lacks a colour/color sense"

    # ── Category filtering ─────────────────────────────────────────────────────
    # For each category, find the first word that is in full AND satisfies the
    # expected tier memberships (to avoid false failures from category overlap).

    offensive_words = load_category("words_offensive.txt")
    adult_words = load_category("words_adult.txt")
    mild_words = load_category("words_mild.txt")

    def find_sample(category_list, predicate, label):
        """Return the first word from category_list that is in full and matches predicate."""
        for w in category_list:
            if w in tiers["full"]["set"] and predicate(w):
                return w
        raise AssertionError(f"No suitable sample word found in {label} category list")

    # Offensive word: in full, NOT in teen (and therefore not in mild/safe either).
    off_sample = find_sample(
        offensive_words,
        lambda w: w not in tiers["teen"]["set"],
        "offensive",
    )
    assert off_sample in tiers["full"]["set"], f"offensive sample '{off_sample}' not in full"
    assert off_sample not in tiers["teen"]["set"], f"offensive sample '{off_sample}' in teen"
    assert off_sample not in tiers["mild"]["set"], f"offensive sample '{off_sample}' in mild"
    assert off_sample not in tiers["safe"]["set"], f"offensive sample '{off_sample}' in safe"
    print(f"offensive '{off_sample}': in full, absent from teen/mild/safe — OK")

    # Adult word: in full AND in teen (i.e. not also in offensive), NOT in mild/safe.
    adult_sample = find_sample(
        adult_words,
        lambda w: w in tiers["teen"]["set"] and w not in tiers["mild"]["set"],
        "adult",
    )
    assert adult_sample in tiers["full"]["set"], f"adult sample '{adult_sample}' not in full"
    assert adult_sample in tiers["teen"]["set"], f"adult sample '{adult_sample}' not in teen"
    assert adult_sample not in tiers["mild"]["set"], f"adult sample '{adult_sample}' in mild"
    assert adult_sample not in tiers["safe"]["set"], f"adult sample '{adult_sample}' in safe"
    print(f"adult     '{adult_sample}': in full+teen, absent from mild/safe — OK")

    # Mild word: in full, teen, and mild (i.e. not also in offensive/adult), NOT in safe.
    mild_sample = find_sample(
        mild_words,
        lambda w: w in tiers["mild"]["set"] and w not in tiers["safe"]["set"],
        "mild",
    )
    assert mild_sample in tiers["full"]["set"], f"mild sample '{mild_sample}' not in full"
    assert mild_sample in tiers["teen"]["set"], f"mild sample '{mild_sample}' not in teen"
    assert mild_sample in tiers["mild"]["set"], f"mild sample '{mild_sample}' not in mild"
    assert mild_sample not in tiers["safe"]["set"], f"mild sample '{mild_sample}' in safe"
    print(f"mild      '{mild_sample}': in full+teen+mild, absent from safe — OK")

    # ── Total size ≤ 14 MB ─────────────────────────────────────────────────────
    size_files = ["dict.dat"] + list(IDX_FILES.values())
    total = sum(os.path.getsize(os.path.join(DATA, f)) for f in size_files)
    assert total <= 14 * 1048576, f"data {total/1048576:.2f}MB exceeds 14 MB budget"

    print(
        f"OK: full={len(tiers['full']['terms'])} teen={len(tiers['teen']['terms'])} "
        f"mild={len(tiers['mild']['terms'])} safe={len(tiers['safe']['terms'])} "
        f"total={total/1048576:.2f}MB"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
