#!/usr/bin/env python3
"""
Build the on-device dictionary from WordNet.

Source : WordNet via nltk (auto-downloaded on first run).
Output : data/dict.dat          (all words, shared by all tiers)
         data/dict_full.idx     (Full  — all words)
         data/dict_teen.idx     (Teen  — excludes offensive)
         data/dict_mild.idx     (Mild  — excludes offensive + adult)
         data/dict_safe.idx     (Safe  — excludes offensive + adult + mild)
         data/ is PlatformIO's filesystem-image source; flash with `pio run -t uploadfs`.

Tier filtering is driven by three curated category word lists under tools/:
  words_offensive.txt  — strong profanity + slurs (excluded from Teen, Mild, Safe)
  words_adult.txt      — sexual / explicit        (excluded from Mild, Safe)
  words_mild.txt       — naughty / slightly rude  (excluded from Safe only)

Format (little-endian, matches the ESP32-S3) -- version 2:

  *.idx
    "DIDX" | u32 version(2) | u32 count | count x [ u32 dataOffset ][ term ][ 0x00 ]
    terms are lowercase a-z, sorted ascending (matches device strcmp binary search)

  dict.dat (shared; all tier indexes point into the same records)
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
    """Load a word category list (one word per line, # comments skipped)."""
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

    # Load the three category word lists.
    offensive = load_blocklist("words_offensive.txt")
    adult = load_blocklist("words_adult.txt")
    mild_list = load_blocklist("words_mild.txt")  # named mild_list to avoid shadowing the tier
    print(f"  category lists: {len(offensive)} offensive, {len(adult)} adult, {len(mild_list)} mild")

    print("Extracting meanings...")
    words = {}
    for w in collect_lemmas(wn):
        ms = meanings_for(w, wn)
        if ms:
            words[w] = ms

    # terms is the FULL set — all WordNet lemmas with at least one meaning.
    terms = sorted(words)

    # Build the four tier subsets (each a strict subset of the next).
    full = terms
    teen = [t for t in terms if t not in offensive]
    mild = [t for t in teen if t not in adult]
    safe = [t for t in mild if t not in mild_list]
    print(f"  full={len(full)}, teen={len(teen)}, mild={len(mild)}, safe={len(safe)} (cap {SENSE_CAP})")

    os.makedirs(OUT_DIR, exist_ok=True)
    dat_path = os.path.join(OUT_DIR, "dict.dat")

    # Write ALL terms to dict.dat and record their offsets.
    # Every tier index looks up offsets in this shared table.
    offsets = {}
    with open(dat_path, "wb") as dat:
        for t in terms:
            offsets[t] = dat.tell()
            ms = words[t]
            dat.write(struct.pack("<B", len(ms)))
            for pc, d, ex in ms:
                db = d.encode("utf-8")[:MAX_DEF]
                eb = ex.encode("utf-8")[:MAX_EX]
                dat.write(struct.pack("<B", pc))
                dat.write(struct.pack("<H", len(db)) + db)
                dat.write(struct.pack("<H", len(eb)) + eb)

    # Write the four tier index files.
    write_idx(os.path.join(OUT_DIR, "dict_full.idx"), full, offsets)
    write_idx(os.path.join(OUT_DIR, "dict_teen.idx"), teen, offsets)
    write_idx(os.path.join(OUT_DIR, "dict_mild.idx"), mild, offsets)
    write_idx(os.path.join(OUT_DIR, "dict_safe.idx"), safe, offsets)

    # Compute sizes and report.
    dat_sz = os.path.getsize(dat_path)
    idx_files = ["dict_full.idx", "dict_teen.idx", "dict_mild.idx", "dict_safe.idx"]
    idx_sizes = {f: os.path.getsize(os.path.join(OUT_DIR, f)) for f in idx_files}
    total = dat_sz + sum(idx_sizes.values())

    print(f"Wrote data/dict.dat      ({dat_sz:,} bytes)")
    for f in idx_files:
        tier_name = f.replace("dict_", "").replace(".idx", "").capitalize()
        print(f"Wrote data/{f:<20} ({idx_sizes[f]:,} bytes)")
    print(f"Total {total / 1048576:.2f} MB. Flash with: pio run -t uploadfs")
    if total > 14 * 1048576:
        print("WARNING: exceeds ~14 MB LittleFS budget; lower DICT_SENSE_CAP.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
