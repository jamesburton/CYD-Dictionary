#!/usr/bin/env python3
"""
Build the on-device dictionary from WordNet.

Source : WordNet via nltk (auto-downloaded on first run).
Output : data/dict.dat (shared), data/dict.idx (Everyone), data/dict_kids.idx (Kids)
         data/ is PlatformIO's filesystem-image source; flash with `pio run -t uploadfs`.

Format (little-endian, matches the ESP32-S3) -- version 2:

  *.idx
    "DIDX" | u32 version(2) | u32 count | count x [ u32 dataOffset ][ term ][ 0x00 ]
    terms are lowercase a-z, sorted ascending (matches device strcmp binary search)

  dict.dat (shared; kids index points into the same records)
    per entry at dataOffset:
      u8 nMeanings
      nMeanings x [ u8 posCode ][ u16 defLen ][ def ][ u16 exLen ][ example ]
    posCode: 0 noun, 1 verb, 2 adjective, 3 adverb, 4 other
"""

import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(os.path.dirname(HERE), "data")

VERSION = 2
SENSE_CAP = int(os.environ.get("DICT_SENSE_CAP", "8"))  # max meanings per word (size lever)
MAX_DEF = 240
MAX_EX = 160
POSMAP = {"n": 0, "v": 1, "a": 2, "s": 2, "r": 3}


def load_blocklist(name):
    path = os.path.join(HERE, name)
    out = set()
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as fh:
            for line in fh:
                w = line.strip().lower()
                if w and not w.startswith("#"):
                    out.add(w)
    return out


def get_wordnet():
    import nltk
    try:
        from nltk.corpus import wordnet as wn
        wn.synsets("test")
    except LookupError:
        nltk.download("wordnet", quiet=True)
        nltk.download("omw-1.4", quiet=True)
        from nltk.corpus import wordnet as wn
        wn.synsets("test")
    return wn


def meanings_for(word, wn):
    """Return [(posCode, def, example), ...] freq-ordered, capped, or []."""
    scored = []
    for idx, syn in enumerate(wn.synsets(word)):
        count = max([l.count() for l in syn.lemmas() if l.name().lower() == word] or [0])
        scored.append((-count, idx, syn))
    scored.sort()
    out = []
    for _, _, syn in scored[:SENSE_CAP]:
        definition = (syn.definition() or "").strip()
        if not definition:
            continue
        example = (syn.examples()[:1] or [""])[0].strip()
        out.append((POSMAP.get(syn.pos(), 4), definition[:MAX_DEF], example[:MAX_EX]))
    return out


def collect_lemmas(wn):
    lemmas = set()
    for syn in wn.all_synsets():
        for lemma in syn.lemmas():
            n = lemma.name()
            if re.fullmatch("[a-z]+", n):
                lemmas.add(n)
    return lemmas


def write_idx(path, words, offsets):
    with open(path, "wb") as f:
        f.write(b"DIDX")
        f.write(struct.pack("<II", VERSION, len(words)))
        for t in words:
            f.write(struct.pack("<I", offsets[t]))
            f.write(t.encode("ascii") + b"\x00")


def main():
    print("Loading WordNet (first run downloads it)...")
    wn = get_wordnet()
    profanity = load_blocklist("blocklist_profanity.txt")
    sensitive = load_blocklist("blocklist_sensitive.txt")
    print(f"  blocklists: {len(profanity)} profanity, {len(sensitive)} sensitive")

    print("Extracting meanings...")
    words = {}
    for w in collect_lemmas(wn):
        ms = meanings_for(w, wn)
        if ms:
            words[w] = ms
    terms = sorted(words)
    everyone = [t for t in terms if t not in profanity]
    kids = [t for t in everyone if t not in sensitive]
    print(f"  total {len(terms)}, everyone {len(everyone)}, kids {len(kids)} (cap {SENSE_CAP})")

    os.makedirs(OUT_DIR, exist_ok=True)
    dat_path = os.path.join(OUT_DIR, "dict.dat")
    idx_path = os.path.join(OUT_DIR, "dict.idx")
    kids_path = os.path.join(OUT_DIR, "dict_kids.idx")

    offsets = {}
    with open(dat_path, "wb") as dat:
        for t in everyone:
            offsets[t] = dat.tell()
            ms = words[t]
            dat.write(struct.pack("<B", len(ms)))
            for pc, d, ex in ms:
                db = d.encode("utf-8")[:MAX_DEF]
                eb = ex.encode("utf-8")[:MAX_EX]
                dat.write(struct.pack("<B", pc))
                dat.write(struct.pack("<H", len(db)) + db)
                dat.write(struct.pack("<H", len(eb)) + eb)

    write_idx(idx_path, everyone, offsets)
    write_idx(kids_path, kids, offsets)

    dat_sz = os.path.getsize(dat_path)
    idx_sz = os.path.getsize(idx_path)
    kids_sz = os.path.getsize(kids_path)
    total = dat_sz + idx_sz + kids_sz
    print(f"Wrote data/dict.dat  ({dat_sz:,} bytes)")
    print(f"Wrote data/dict.idx  ({idx_sz:,} bytes, {len(everyone)} words)")
    print(f"Wrote data/dict_kids.idx ({kids_sz:,} bytes, {len(kids)} words)")
    print(f"Total {total/1048576:.1f} MB. Flash with: pio run -t uploadfs")
    if total > 14 * 1048576:
        print("WARNING: exceeds ~14 MB LittleFS budget; lower DICT_SENSE_CAP.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
