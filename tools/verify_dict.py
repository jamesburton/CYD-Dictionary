#!/usr/bin/env python3
"""
Parse data/dicts/base.idx + base.dat + base.meta (format v4) and assert correctness.
Exits non-zero on any failure.

Checks:
  - Index magic "DIDX" and version == 4.
  - Keys are sorted ascending.
  - For each entry, wordMinTier == min(meaning.minTier).
  - Every meaning has minTier <= maxTier.
  - Display headword is non-empty.
  - Total file size (base.idx + base.dat + base.meta) <= 14 MB.
  - Spot-checks: blue/toy/mouse primary senses print correctly.
  - "fuck" (an offensive headword) has wordMinTier == FULL.
  - If any override-appended meanings exist (maxTier < FULL), at least one
    is visible at SAFE (minTier==SAFE, maxTier>=SAFE).  Skipped when absent.
  - Simulates per-tier visible-word counts (wordMinTier <= activeTier).
"""

import os
import struct
import sys

from normalize import norm_key

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(os.path.dirname(HERE), "data", "dicts")

POS = {0: "noun", 1: "verb", 2: "adjective", 3: "adverb", 4: "other"}

# Tier constants — must match build_dict.py.
SAFE = 0
MILD = 1
TEEN = 2
FULL = 3
TIER_NAMES = {SAFE: "Safe", MILD: "Mild", TEEN: "Teen", FULL: "Full"}


def read_idx(path):
    """
    Parse a v4 index file.
    Returns (keys, data_offsets, word_min_tiers) — all parallel lists.
    """
    with open(path, "rb") as fh:
        b = fh.read()

    assert b[:4] == b"DIDX", f"{path}: bad magic (got {b[:4]!r})"
    version, count = struct.unpack_from("<II", b, 4)
    assert version == 4, f"{path}: version {version} != 4"

    keys = []
    data_offsets = []
    word_min_tiers = []
    p = 12

    for _ in range(count):
        off = struct.unpack_from("<I", b, p)[0]; p += 4
        wmt = b[p]; p += 1                                # u8 wordMinTier
        nul = b.index(b"\x00", p)
        keys.append(b[p:nul].decode("ascii"))             # normalised key
        data_offsets.append(off)
        word_min_tiers.append(wmt)
        p = nul + 1

    return keys, data_offsets, word_min_tiers


def read_entry(dat_bytes, off):
    """
    Parse one base.dat entry (v4).
    Returns (display, list of (posCode, definition, example, minTier, maxTier)).
    """
    p = off
    disp_len = dat_bytes[p]; p += 1
    display = dat_bytes[p:p + disp_len].decode("utf-8", "replace"); p += disp_len
    n = dat_bytes[p]; p += 1
    meanings = []
    for _ in range(n):
        tier_range = dat_bytes[p]; p += 1                  # u8 tierRange
        min_tier = tier_range & 0x03
        max_tier = (tier_range >> 2) & 0x03
        pc = dat_bytes[p]; p += 1                          # u8 posCode
        dl = struct.unpack_from("<H", dat_bytes, p)[0]; p += 2
        d = dat_bytes[p:p + dl].decode("utf-8", "replace"); p += dl
        el = struct.unpack_from("<H", dat_bytes, p)[0]; p += 2
        ex = dat_bytes[p:p + el].decode("utf-8", "replace"); p += el
        meanings.append((pc, d, ex, min_tier, max_tier))
    return display, meanings


def main():
    idx_path = os.path.join(DATA, "base.idx")
    dat_path = os.path.join(DATA, "base.dat")
    meta_path = os.path.join(DATA, "base.meta")

    # ── Load index ─────────────────────────────────────────────────────────────
    print(f"Reading {idx_path}")
    keys, data_offsets, idx_word_min_tiers = read_idx(idx_path)
    print(f"  {len(keys)} entries, version 4 — OK")

    # ── Keys sorted ascending ──────────────────────────────────────────────────
    assert keys == sorted(keys), "base.idx: keys are not sorted ascending"
    print("  sorted — OK")

    # ── Load dat ───────────────────────────────────────────────────────────────
    print(f"Reading {dat_path}")
    with open(dat_path, "rb") as fh:
        dat = fh.read()
    print(f"  {len(dat):,} bytes")

    # ── Per-entry consistency ──────────────────────────────────────────────────
    print("Checking per-entry consistency...")
    errors = []
    override_meanings = []  # meanings with maxTier < FULL (appended by overrides.tsv)

    for i, (key, off, idx_wmt) in enumerate(zip(keys, data_offsets, idx_word_min_tiers)):
        try:
            display, meanings = read_entry(dat, off)
        except Exception as exc:
            errors.append(f"  '{key}': failed to parse entry at offset {off}: {exc}")
            continue

        if not display:
            errors.append(f"  '{key}': display headword is empty")

        if not meanings:
            errors.append(f"  '{key}': nMeanings == 0")
            continue

        for j, (pc, d, ex, min_t, max_t) in enumerate(meanings):
            if min_t > max_t:
                errors.append(f"  '{key}' meaning {j}: minTier {min_t} > maxTier {max_t}")
            if max_t < FULL:
                override_meanings.append((key, j, min_t, max_t))

        computed_wmt = min(min_t for (_, _, _, min_t, _) in meanings)
        if computed_wmt != idx_wmt:
            errors.append(
                f"  '{key}': idx wordMinTier={idx_wmt} but min(meaning.minTier)={computed_wmt}"
            )

    if errors:
        for e in errors[:20]:
            print(e, file=sys.stderr)
        if len(errors) > 20:
            print(f"  ... and {len(errors) - 20} more errors", file=sys.stderr)
        sys.exit(f"FAIL: {len(errors)} consistency error(s)")

    print(f"  all {len(keys)} entries consistent — OK")

    # ── Total size <= 14 MB ────────────────────────────────────────────────────
    meta_sz = os.path.getsize(meta_path) if os.path.exists(meta_path) else 0
    total = os.path.getsize(idx_path) + os.path.getsize(dat_path) + meta_sz
    assert total <= 14 * 1048576, f"Total {total/1048576:.2f} MB exceeds 14 MB LittleFS budget"
    print(f"  total size {total/1048576:.2f} MB — OK")

    # Build a key→(offset, wordMinTier) map for spot-checks.
    key_map = {k: (off, wmt) for k, off, wmt in zip(keys, data_offsets, idx_word_min_tiers)}

    # ── Primary-sense spot-checks: blue / toy / mouse ──────────────────────────
    for w in ("blue", "toy", "mouse"):
        nk = norm_key(w)
        assert nk in key_map, f"'{w}' (key='{nk}') missing from base.idx"
        off, _ = key_map[nk]
        display, ms = read_entry(dat, off)
        pc, d, ex, min_t, max_t = ms[0]
        print(f"  {display}: [{POS.get(pc, pc)}] {d[:60]}")

    blue_off, _ = key_map[norm_key("blue")]
    _, blue_meanings = read_entry(dat, blue_off)
    blue_defs = " ".join(d for _, d, _, _, _ in blue_meanings).lower()
    assert "colour" in blue_defs or "color" in blue_defs, "blue lacks a colour/color sense"
    print("  blue has colour/color sense — OK")

    # ── Offensive headword spot-check: "fuck" must have wordMinTier == FULL ────
    fuck_key = norm_key("fuck")
    assert fuck_key in key_map, "'fuck' missing from base.idx (check words_offensive.txt)"
    _, fuck_wmt = key_map[fuck_key]
    assert fuck_wmt == FULL, f"'fuck' wordMinTier={fuck_wmt} but expected FULL ({FULL})"
    print(f"  'fuck' wordMinTier == FULL — OK")

    # ── Override spot-check: if any override meanings exist, one must be at SAFE ─
    if override_meanings:
        safe_overrides = [(t, j, mn, mx) for (t, j, mn, mx) in override_meanings if mn == SAFE]
        assert safe_overrides, (
            f"Found {len(override_meanings)} override meaning(s) (maxTier < FULL) "
            "but none have minTier == SAFE"
        )
        sample = safe_overrides[0]
        print(
            f"  override spot-check: '{sample[0]}' meaning {sample[1]} "
            f"visible at SAFE (minTier={sample[2]}, maxTier={sample[3]}) — OK"
        )
    else:
        print("  override spot-check: no override meanings present — skipped")

    # ── Simulated per-tier visible word counts ─────────────────────────────────
    tier_counts = {t: sum(1 for wmt in idx_word_min_tiers if wmt <= t) for t in (SAFE, MILD, TEEN, FULL)}
    print(
        f"\nVisible word counts by active tier:"
        f"\n  Safe : {tier_counts[SAFE]:,}"
        f"\n  Mild : {tier_counts[MILD]:,}"
        f"\n  Teen : {tier_counts[TEEN]:,}"
        f"\n  Full : {tier_counts[FULL]:,}"
    )
    print(
        f"\nOK: {len(keys)} total entries, {total/1048576:.2f} MB, SENSE_CAP=8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
