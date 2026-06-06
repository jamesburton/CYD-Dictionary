#!/usr/bin/env python3
"""
Build the on-SD dictionary files (dict.idx + dict.dat) from the Wordset corpus.

Source : tools/wordset/data/*.json  (clone of github.com/wordset/wordset-dictionary)
Output : sdcard/dict.idx, sdcard/dict.dat   (copy both to the SD card root, FAT32)

Format (all integers little-endian, matching the ESP32-S3):

  dict.idx
    magic   : 4 bytes  b"DIDX"
    version : u32       (1)
    count   : u32       number of entries
    records : count *   [ u32 dataOffset ][ term bytes ][ 0x00 ]
                        terms are lowercase a-z only, sorted ascending so that
                        the device's strcmp binary search matches Python sorted()

  dict.dat
    per entry, at dataOffset:
      [ u8  posLen ][ pos bytes ]
      [ u16 defLen ][ def bytes ]
      [ u16 exLen  ][ example bytes ]
"""

import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.path.join(HERE, "wordset", "data")
OUT_DIR = os.path.join(os.path.dirname(HERE), "sdcard")

MAGIC = b"DIDX"
VERSION = 1

MAX_DEF = 600        # clamp pathological definitions (u16 field, keeps reads small)
MAX_EX = 400


def is_clean_term(term: str) -> bool:
    """Accept only lowercase a-z. Guarantees term[i]-'a' is in range on device
    and that Python's sorted() order equals the device's byte-wise strcmp."""
    return len(term) > 0 and all("a" <= ch <= "z" for ch in term)


def pick_meaning(entry: dict):
    """Return (pos, definition, example) from the best available meaning, or None."""
    for m in entry.get("meanings", []):
        definition = (m.get("def") or "").strip()
        if not definition:
            continue
        pos = (m.get("speech_part") or "").strip()
        example = (m.get("example") or "").strip()
        return pos, definition[:MAX_DEF], example[:MAX_EX]
    return None


def collect() -> dict:
    """word -> (pos, def, example), deduped and cleaned."""
    words = {}
    skipped = 0
    for name in sorted(os.listdir(DATA_DIR)):
        if not name.endswith(".json"):
            continue
        with open(os.path.join(DATA_DIR, name), "r", encoding="utf-8") as fh:
            blob = json.load(fh)
        for raw_word, entry in blob.items():
            term = raw_word.strip().lower()
            if not is_clean_term(term):
                skipped += 1
                continue
            picked = pick_meaning(entry)
            if picked is None:
                skipped += 1
                continue
            if term not in words:        # first clean meaning wins
                words[term] = picked
    print(f"  collected {len(words)} words, skipped {skipped}")
    return words


def main() -> int:
    if not os.path.isdir(DATA_DIR):
        print(f"ERROR: corpus not found at {DATA_DIR}", file=sys.stderr)
        print("Clone it first: git clone --depth 1 "
              "https://github.com/wordset/wordset-dictionary.git tools/wordset",
              file=sys.stderr)
        return 1

    print("Reading Wordset corpus...")
    words = collect()
    terms = sorted(words.keys())        # ascending; matches device strcmp order

    os.makedirs(OUT_DIR, exist_ok=True)
    idx_path = os.path.join(OUT_DIR, "dict.idx")
    dat_path = os.path.join(OUT_DIR, "dict.dat")

    # Pass 1: build dict.dat and record each entry's offset.
    offsets = {}
    with open(dat_path, "wb") as dat:
        for term in terms:
            pos, definition, example = words[term]
            offsets[term] = dat.tell()
            pb = pos.encode("utf-8")[:255]
            db = definition.encode("utf-8")[:MAX_DEF]
            eb = example.encode("utf-8")[:MAX_EX]
            dat.write(struct.pack("<B", len(pb)) + pb)
            dat.write(struct.pack("<H", len(db)) + db)
            dat.write(struct.pack("<H", len(eb)) + eb)

    # Pass 2: build dict.idx.
    with open(idx_path, "wb") as idx:
        idx.write(MAGIC)
        idx.write(struct.pack("<II", VERSION, len(terms)))
        for term in terms:
            idx.write(struct.pack("<I", offsets[term]))
            idx.write(term.encode("ascii") + b"\x00")

    idx_sz = os.path.getsize(idx_path)
    dat_sz = os.path.getsize(dat_path)
    print(f"Wrote {idx_path}  ({idx_sz:,} bytes)")
    print(f"Wrote {dat_path}  ({dat_sz:,} bytes)")
    print(f"Done: {len(terms):,} words. Copy both files to the SD card root (FAT32).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
