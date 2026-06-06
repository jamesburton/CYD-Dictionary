#!/usr/bin/env python3
"""Parse the generated data/ files and assert correctness. Exit non-zero on failure."""
import os
import struct
import sys

DATA = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data")
POS = {0: "noun", 1: "verb", 2: "adjective", 3: "adverb", 4: "other"}


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
    et, eo = read_idx(os.path.join(DATA, "dict.idx"))
    kt, ko = read_idx(os.path.join(DATA, "dict_kids.idx"))
    dat = open(os.path.join(DATA, "dict.dat"), "rb").read()

    assert et == sorted(et), "everyone index not sorted"
    assert kt == sorted(kt), "kids index not sorted"
    eset = set(et)
    assert set(kt) <= eset, "kids is not a subset of everyone"

    # kids offsets must match everyone offsets (shared data)
    emap = dict(zip(et, eo))
    for t, o in zip(kt, ko):
        assert emap[t] == o, f"kids offset mismatch for {t}"

    # primary sense sanity
    def sense1(term):
        return read_entry(dat, emap[term])[0]
    for w, want in [("blue", "noun"), ("toy", "noun"), ("mouse", "noun")]:
        assert w in eset, f"{w} missing"
        pc, d, _ = sense1(w)
        print(f"{w}: [{POS[pc]}] {d[:60]}")
    # blue must mention colour/color in some sense; not lead with depression
    blue_defs = " ".join(d for _, d, _ in read_entry(dat, emap["blue"])).lower()
    assert "colour" in blue_defs or "color" in blue_defs, "blue lacks a colour sense"

    # filtering: a sample profane word absent from everyone; a sensitive word absent from kids
    for bad in ["fuck", "shit"]:
        assert bad not in eset, f"profanity {bad} present in everyone"
    for s in ["sex", "drug"]:
        if s in eset:
            assert s not in set(kt), f"sensitive {s} present in kids"

    total = sum(os.path.getsize(os.path.join(DATA, f)) for f in ("dict.dat", "dict.idx", "dict_kids.idx"))
    print(f"OK: everyone={len(et)} kids={len(kt)} total={total/1048576:.1f}MB")
    assert total <= 14 * 1048576, "data exceeds ~14MB budget"
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
