#!/usr/bin/env python3
"""
Build the on-device base dictionary from WordNet (format v4).

Source : WordNet via nltk (auto-downloaded on first run).
Output : data/dicts/base.idx    (single index; entry includes wordMinTier)
         data/dicts/base.dat    (all words with display headword + per-meaning tierRange)
         data/dicts/base.meta   (name/version/mode/floor/format=4)

Tier constants (SAFE=0, MILD=1, TEEN=2, FULL=3):
  - wordMinTier = min(minTier) over all meanings; written in the index.
  - tierRange   = minTier | (maxTier<<2); written per-meaning in dat.
  - A meaning is visible iff minTier <= activeTier <= maxTier.

Auto tier floors are driven by four curated word lists under tools/:
  words_offensive.txt  — strong profanity + slurs  → headword floor FULL
  words_adult.txt      — explicit sexual content    → headword floor TEEN
  words_mild.txt       — mild swears / toilet humour → headword floor MILD
  core_harmful.txt     — tokens unacceptable inside a definition gloss

Optional input files (tools/, TSV, skipped gracefully if absent):
  sense_labels.tsv     word<TAB>def_prefix<TAB>minTierName
  overrides.tsv        word<TAB>def_prefix<TAB>sanitised_def[<TAB>sanitised_ex]

Format v4 (little-endian, matches firmware):

  data/dicts/base.idx
    "DIDX" | u32 version=4 | u32 count
    per entry: [u32 dataOffset][u8 wordMinTier][normalised-key ascii][0x00]
    keys sorted ascending.

  data/dicts/base.dat
    per entry at dataOffset:
      u8 dispLen + display bytes (display headword)
      u8 nMeanings
      nMeanings x [u8 tierRange][u8 posCode][u16 defLen][def][u16 exLen][example]
    tierRange = minTier | (maxTier<<2)  (each 2 bits, 0..3)
    posCode: 0 noun, 1 verb, 2 adjective, 3 adverb, 4 other

  data/dicts/base.meta
    text key=value lines: name, version, mode, floor, format=4
"""

import os
import re
import struct
import sys

from normalize import norm_key

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(os.path.dirname(HERE), "data", "dicts")

VERSION = 4
SENSE_CAP = int(os.environ.get("DICT_SENSE_CAP", "8"))  # max meanings per word (size lever)
MAX_DEF = 240
MAX_EX = 160
POSMAP = {"n": 0, "v": 1, "a": 2, "s": 2, "r": 3}

# Tier constants.
SAFE = 0
MILD = 1
TEEN = 2
FULL = 3
TIER_NAMES = {"safe": SAFE, "mild": MILD, "teen": TEEN, "full": FULL}


def load_wordlist(name):
    """Load a word list file (one word per line, # comments skipped). Returns a set."""
    path = os.path.join(HERE, name)
    out = set()
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as fh:
            for line in fh:
                w = line.strip().lower()
                if w and not w.startswith("#"):
                    out.add(w)
    return out


def load_tsv(name):
    """Load a TSV file. Returns list of rows (each a list of stripped strings), skips # lines."""
    path = os.path.join(HERE, name)
    rows = []
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as fh:
            for line in fh:
                line = line.rstrip("\n")
                if not line or line.startswith("#"):
                    continue
                rows.append([c.strip() for c in line.split("\t")])
    return rows


def get_wordnet():
    """Return the WordNet corpus handle, downloading if needed."""
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


def headword_floor(word, offensive, adult, mild_list):
    """Return the tier floor imposed by which category lists the headword appears in."""
    if word in offensive:
        return FULL
    if word in adult:
        return TEEN
    if word in mild_list:
        return MILD
    return SAFE


def core_gloss_floor(text, offensive, gloss_core):
    """
    Return the tier floor implied by harmful tokens appearing in a definition/example.
    gloss_core = core_harmful minus {retard, retards, retarded}.
    Each token in (text_tokens & gloss_core) contributes FULL if it is offensive, else TEEN.
    """
    tokens = set(re.findall(r"[a-z]+", text.lower()))
    found = tokens & gloss_core
    if not found:
        return SAFE
    result = SAFE
    for tok in found:
        result = max(result, FULL if tok in offensive else TEEN)
    return result


def meanings_for(word, wn, offensive, adult, mild_list, gloss_core):
    """
    Return [(posCode, definition, example, minTier, maxTier), ...] freq-ordered, capped.
    minTier = max(headword_floor, core_gloss_floor(def + " " + ex)).
    maxTier = FULL for all WordNet-sourced meanings.
    """
    hw_floor = headword_floor(word, offensive, adult, mild_list)

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
        def_trunc = definition[:MAX_DEF]
        ex_trunc = example[:MAX_EX]

        gloss_floor = core_gloss_floor(def_trunc + " " + ex_trunc, offensive, gloss_core)
        min_tier = max(hw_floor, gloss_floor)
        out.append((POSMAP.get(syn.pos(), 4), def_trunc, ex_trunc, min_tier, FULL))
    return out


def collect_lemmas(wn):
    """Return the set of all single-word (a-z only) lemmas in WordNet."""
    lemmas = set()
    for syn in wn.all_synsets():
        for lemma in syn.lemmas():
            n = lemma.name()
            if re.fullmatch("[a-z]+", n):
                lemmas.add(n)
    return lemmas


def apply_sense_labels(words, sense_labels, offensive, adult, mild_list):
    """
    Apply sense_labels.tsv: for each row (word, def_prefix, minTierName), find the
    matching meaning and raise its minTier to at least the named value.
    """
    for row in sense_labels:
        if len(row) < 3:
            continue
        word, def_prefix, tier_name = row[0].lower(), row[1], row[2].lower()
        named_tier = TIER_NAMES.get(tier_name)
        if named_tier is None:
            print(f"  sense_labels: unknown tier '{tier_name}' for '{word}' — skipped", file=sys.stderr)
            continue
        if word not in words:
            continue
        meanings = words[word]
        matched = False
        for i, (pc, d, ex, min_t, max_t) in enumerate(meanings):
            if d.startswith(def_prefix):
                new_min = max(min_t, named_tier)
                meanings[i] = (pc, d, ex, new_min, max_t)
                matched = True
                break
        if not matched:
            print(f"  sense_labels: no match for '{word}' / '{def_prefix[:40]}' — skipped", file=sys.stderr)


def apply_overrides(words, override_rows, offensive, adult, mild_list):
    """
    Apply overrides.tsv: for each matched meaning (post-labels minTier = M), append a
    new sanitised meaning with minTier = headword_floor(word) and maxTier = M-1.
    Skips (with log) if M-1 < headword_floor(word).
    """
    for row in override_rows:
        if len(row) < 3:
            continue
        word = row[0].lower()
        def_prefix = row[1]
        sanitised_def = row[2][:MAX_DEF]
        sanitised_ex = row[3][:MAX_EX] if len(row) > 3 else ""

        if word not in words:
            continue
        meanings = words[word]
        hw_floor = headword_floor(word, offensive, adult, mild_list)

        for i, (pc, d, ex, min_t, max_t) in enumerate(meanings):
            if d.startswith(def_prefix):
                M = min_t  # post-label minTier
                if M - 1 < hw_floor:
                    print(
                        f"  overrides: skip '{word}' / '{def_prefix[:40]}': "
                        f"M-1={M-1} < headword_floor={hw_floor}",
                        file=sys.stderr,
                    )
                    break
                # Append a sanitised sibling with minTier=hw_floor, maxTier=M-1.
                words[word].append((pc, sanitised_def, sanitised_ex, hw_floor, M - 1))
                break


def write_idx(path, words, offsets, wordmin):
    """Write a v4 index file: DIDX | u32 version=4 | u32 count | entries (normalised key)."""
    with open(path, "wb") as f:
        f.write(b"DIDX"); f.write(struct.pack("<II", VERSION, len(words)))
        for w in words:
            f.write(struct.pack("<I", offsets[w]))
            f.write(struct.pack("<B", wordmin[w]))
            f.write(norm_key(w).encode("ascii") + b"\x00")   # key


def write_meta(path, name, mode, floor):
    """Write a .meta file (text key=value) for the dictionary."""
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"name={name}\nversion=dict-2026.06\nmode={mode}\nfloor={floor}\nformat=4\n")


def main():
    print("Loading WordNet (first run downloads it)...")
    wn = get_wordnet()

    # Load the four category word lists.
    offensive = load_wordlist("words_offensive.txt")
    adult = load_wordlist("words_adult.txt")
    mild_list = load_wordlist("words_mild.txt")
    core_harmful = load_wordlist("core_harmful.txt")
    print(
        f"  category lists: {len(offensive)} offensive, {len(adult)} adult, "
        f"{len(mild_list)} mild, {len(core_harmful)} core_harmful"
    )

    # gloss_core: core_harmful minus the retard* exception (those words gate the headword
    # but must not raise the tier of innocent definitions that use the word medically).
    retard_exceptions = {"retard", "retards", "retarded"}
    gloss_core = core_harmful - retard_exceptions

    # Load optional TSV files.
    sense_labels = load_tsv("sense_labels.tsv")
    override_rows = load_tsv("overrides.tsv")
    print(
        f"  sense_labels: {len(sense_labels)} rows, overrides: {len(override_rows)} rows"
        + (" (files may be absent — handled gracefully)" if not sense_labels and not override_rows else "")
    )

    print("Extracting meanings from WordNet...")
    words = {}
    for w in collect_lemmas(wn):
        ms = meanings_for(w, wn, offensive, adult, mild_list, gloss_core)
        if ms:
            words[w] = ms

    print(f"  extracted {len(words)} lemmas (cap {SENSE_CAP})")

    # Apply sense_labels (raise minTier for specific senses).
    if sense_labels:
        apply_sense_labels(words, sense_labels, offensive, adult, mild_list)
        print(f"  applied {len(sense_labels)} sense_label rows")

    # Apply overrides (append sanitised sibling meanings).
    if override_rows:
        apply_overrides(words, override_rows, offensive, adult, mild_list)
        print(f"  applied {len(override_rows)} override rows")

    # Compute wordMinTier = min(minTier) over all meanings (incl. appended).
    word_min_tiers = {}
    for w, ms in words.items():
        word_min_tiers[w] = min(min_t for (_, _, _, min_t, _) in ms)

    # Full sorted term list (single index, all words).
    terms = sorted(words)

    os.makedirs(OUT_DIR, exist_ok=True)
    dat_path = os.path.join(OUT_DIR, "base.dat")

    # Write all terms to base.dat and record their offsets.
    offsets = {}
    with open(dat_path, "wb") as dat:
        for t in terms:
            offsets[t] = dat.tell()
            ms = words[t]
            disp = t.encode("utf-8")[:255]
            dat.write(struct.pack("<B", len(disp)) + disp)   # display headword
            dat.write(struct.pack("<B", len(ms)))   # u8 nMeanings
            for pc, d, ex, min_t, max_t in ms:
                tier_range = min_t | (max_t << 2)
                db = d.encode("utf-8")[:MAX_DEF]
                eb = ex.encode("utf-8")[:MAX_EX]
                dat.write(struct.pack("<B", tier_range))   # u8 tierRange
                dat.write(struct.pack("<B", pc))           # u8 posCode
                dat.write(struct.pack("<H", len(db)) + db) # u16 defLen + def
                dat.write(struct.pack("<H", len(eb)) + eb) # u16 exLen + example

    # Write the v4 index file.
    idx_path = os.path.join(OUT_DIR, "base.idx")
    write_idx(idx_path, terms, offsets, word_min_tiers)

    # Write the meta file.
    meta_path = os.path.join(OUT_DIR, "base.meta")
    write_meta(meta_path, "Base (WordNet)", "additive", "safe")

    # Report sizes.
    dat_sz = os.path.getsize(dat_path)
    idx_sz = os.path.getsize(idx_path)
    meta_sz = os.path.getsize(meta_path)
    total = dat_sz + idx_sz + meta_sz

    # Simulated per-tier visible word counts (wordMinTier <= activeTier).
    tier_counts = {t: sum(1 for wmt in word_min_tiers.values() if wmt <= t) for t in (SAFE, MILD, TEEN, FULL)}

    print(f"Wrote data/dicts/base.dat  ({dat_sz:,} bytes)")
    print(f"Wrote data/dicts/base.idx  ({idx_sz:,} bytes)")
    print(f"Wrote data/dicts/base.meta ({meta_sz:,} bytes)")
    print(f"Total {total / 1048576:.2f} MB.  Flash with: pio run -t uploadfs")
    print(
        f"Visible at tiers — Safe: {tier_counts[SAFE]:,}  Mild: {tier_counts[MILD]:,}  "
        f"Teen: {tier_counts[TEEN]:,}  Full: {tier_counts[FULL]:,}  (SENSE_CAP={SENSE_CAP})"
    )

    if total > 14 * 1048576:
        print(
            "WARNING: total exceeds ~14 MB LittleFS budget; lower DICT_SENSE_CAP or trim lists.",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
