# Dictionaries

This document covers the multi-source dictionary engine introduced in v1.3:
the on-device layout, binary formats, exclusion files, how to author and install
supplementary dictionaries, the base-build pipeline, content classification, and
the Dictionaries settings UI.

---

## On-device layout (`/dicts/`)

The device loads dictionaries from a `/dicts/` folder. Both LittleFS (built-in
flash) and a FAT32 SD card are scanned. Each dictionary consists of three files
sharing the same stem:

```
/dicts/
    base.idx
    base.dat
    base.meta
    mythology.idx
    mythology.dat
    mythology.meta
    exclude/
        extra-rude.excl
```

### Flash vs SD

Flash (LittleFS) is scanned first, then SD. If the SD card contains a dictionary
whose **filename stem** (e.g. `mythology`) matches one already loaded from flash,
the **SD version shadows the flash version** — SD wins for that dictionary. New
stems on the SD card are added alongside the flash dictionaries.

This means you can test or distribute an updated dictionary by copying three
files to a FAT32 SD card, without reflashing the device.

### Exclusion files

Any `.excl` file placed under `/dicts/exclude/` (on flash or SD) is loaded as a
runtime exclusion list. If the same filename appears on both flash and SD, the
flash copy wins (first loaded, duplicate skipped).

---

## Format v4 (little-endian)

All three files use little-endian byte order. Tiers are integers:
`SAFE=0, MILD=1, TEEN=2, FULL=3`.

### `<name>.idx` — index file

```
"DIDX"            4 bytes   magic
u32 version       must be 4
u32 count         number of entries

per entry (count times):
    u32 dataOffset    byte offset into the .dat file
    u8  wordMinTier   min(minTier) over all meanings for this entry
    [normalised search key, ASCII, null-terminated]
```

Entries are sorted ascending by search key. The entire `.idx` is loaded into
PSRAM at boot and parsed to parallel `key[]`, `dataOffset[]`, `wordMinTier[]`
arrays — no `.dat` read is needed for browse or prefix search.

### `<name>.dat` — word data file

```
per entry at dataOffset:
    u8  dispLen         byte length of the display headword
    [display headword, UTF-8, dispLen bytes]
    u8  nMeanings

    nMeanings times:
        u8  tierRange   minTier | (maxTier << 2)   (2 bits each, values 0–3)
        u8  posCode     0=noun 1=verb 2=adjective 3=adverb 4=other/name
        u16 defLen
        [definition, UTF-8, defLen bytes]
        u16 exLen
        [example, UTF-8, exLen bytes; 0 if no example]
```

A meaning is shown when `minTier <= activeTier <= maxTier`. `tierRange` packs both
bounds: `minTier = tierRange & 0x03`, `maxTier = (tierRange >> 2) & 0x03`.

### `<name>.meta` — metadata file

A plain-text `key=value` file:

```
name=Base (WordNet)
version=dict-2026.06
mode=additive
floor=safe
format=4
```

Keys:

| Key | Values | Meaning |
|-----|--------|---------|
| `name` | display string | Shown in the Dictionaries UI |
| `version` | stamp | Dataset version (informational) |
| `mode` | `additive` \| `override` | Merge behaviour (see below) |
| `floor` | `safe` \| `mild` \| `teen` \| `full` | Minimum tier for the whole dictionary |
| `format` | `4` | Must be 4; non-4 is rejected |

**Merge modes:**
- **additive** — this dictionary's meanings are appended on top of lower-priority
  dictionaries for the same key; lower sources keep contributing.
- **override** — this dictionary's meanings replace those of all lower-priority
  dictionaries for the same key; lower sources are stopped for that word.

The merge walk goes highest-priority → lowest. A dictionary lacking a key passes
through silently; only dictionaries that contain the key participate in the merge.

### Search-key normalisation

The search key stored in `.idx` (and used for all lookup/merge matching) is
derived by the same rule in Python (`tools/normalize.py: norm_key`) and C++
(`src/Dict.cpp: dictNormalizeKey`):

1. NFKD decompose; strip combining characters (strip diacritics to ASCII).
2. Lowercase.
3. Drop apostrophes (`'` and `'`).
4. Replace any character not in `[a-z0-9 -]` with a space.
5. Collapse runs of whitespace to a single space; trim leading/trailing space.

Examples: `"Achilles' heel"` → `"achilles heel"`, `"Mount Olympus"` →
`"mount olympus"`, `"café"` → `"cafe"`, `"well-known"` → `"well-known"`.
The hyphen is kept; apostrophes are dropped (typing `achilles heel` finds
`Achilles' heel`).

---

## Exclusion file format (`.excl`)

Exclusion files live under `/dicts/exclude/`. Each file begins with two
directive comment lines, followed by one normalised search key per line.

```
# action: gate:full
# scope: all
shitshow
arsehattery
```

**Directive lines** (must appear in this order, each starting with `# `):

| Directive | Values | Effect |
|-----------|--------|--------|
| `action:` | `hide` | Remove the key from the merged view entirely |
| | `gate:<tier>` | Raise the key's effective min tier to `<tier>` (e.g. `gate:full` hides below Full) |
| `scope:` | `all` | Apply to all loaded dictionaries |
| | `id1,id2,...` | Apply only to the named dictionary stems (e.g. `base,mythology`) |

Remaining non-comment lines are treated as keys; each is normalised on load (same
`norm_key` rules as above). An enabled exclusion overrides normal tier visibility.

Exclusion files are toggleable per-file in the Dictionaries settings UI; the
enabled state persists in NVS. A sample file is provided at
`tools/sample-exclusions/extra-rude.excl`.

---

## Authoring a supplementary dictionary

### JSON source schema

Create a JSON file containing an array of entry objects:

```json
[
  {
    "term": "Zeus",
    "pos": "name",
    "tier": "safe",
    "defs": [
      "King of the Greek gods and ruler of Mount Olympus, god of the sky, thunder, and law.",
      "Youngest child of the Titans Cronus and Rhea; brother and husband of Hera."
    ],
    "example": "Zeus hurled a thunderbolt at the giants who stormed Olympus."
  },
  {
    "term": "Mount Olympus",
    "pos": "name",
    "tier": "safe",
    "defs": ["The highest mountain in Greece, home of the twelve Olympian gods."]
  }
]
```

| Field | Type | Required | Notes |
|-------|------|----------|-------|
| `term` | string | yes | Display headword, any case/accents/spaces |
| `pos` | string | yes | `noun`, `verb`, `adjective`, `adverb`, `name`, `other` |
| `tier` | string | no | `safe` (default), `mild`, `teen`, `full` — the minimum tier for all of this entry's meanings |
| `defs` | array of strings | yes | One string per definition; at least one required |
| `example` | string | no | Optional example sentence; attached to the **first** meaning only |

Notes:
- The `tier` value applies to **all meanings** of the entry and sets `minTier`; `maxTier` is always `FULL` for supplementary entries.
- `pos` `"name"` maps to `posCode 4` (displayed as `other`). There is no separate named-entity part-of-speech rendering.
- Definitions are stored up to 600 bytes each; examples up to 400 bytes (UTF-8). These caps are looser than the base-build caps (240/160 bytes) to accommodate longer supplementary entries.
- Write original definitions; do not copy licensed encyclopedia text.

### Building

Run from the project root:

```sh
python tools/build_supp.py tools/sources/mythology.json mythology --mode additive --floor safe
```

Arguments:

| Argument | Description |
|----------|-------------|
| `<source.json>` | Path to the JSON source file |
| `<name>` | Output stem; also becomes the dictionary id (used for SD shadowing and scope matching in `.excl`) |
| `--mode additive\|override` | Merge mode written to `.meta` (default: `additive`) |
| `--floor safe\|mild\|teen\|full` | Whole-dictionary tier floor written to `.meta` (default: `safe`) |

Outputs three files into `data/dicts/`:
```
data/dicts/mythology.idx
data/dicts/mythology.dat
data/dicts/mythology.meta
```

The `data/` directory is git-ignored; commit the source JSON and script only.

### Installing

**Via flash (LittleFS):** Place the source JSON under `tools/sources/`, run
`build_supp.py`, then reflash the filesystem:

```sh
pio run -t uploadfs
```

This flashes the entire `data/` directory contents, including all files in
`data/dicts/`, to the LittleFS partition.

**Important:** if you change which files are in `data/dicts/` (add, remove, or
rename), purge any stale files from `data/dicts/` before running `uploadfs`.
LittleFS images are generated from the directory contents; stale files increase
the image size and can cause the 14.4 MB partition to overflow.

**Via SD card (no reflash):** Copy the three files (`<name>.idx`, `<name>.dat`,
`<name>.meta`) to a FAT32 SD card under `/dicts/`:

```
/dicts/mythology.idx
/dicts/mythology.dat
/dicts/mythology.meta
```

Insert the card; the engine discovers and loads the dictionary at the next boot.
If a same-named dictionary exists in flash, the SD version shadows it.

---

## Building the base dictionary

### Prerequisites

```sh
pip install nltk wordfreq
```

WordNet data is downloaded automatically on the first `build_dict.py` run
(requires internet access once).

### Generate

```sh
python tools/build_dict.py
```

Writes to `data/dicts/`:

| File | Contents |
|------|----------|
| `data/dicts/base.idx` | Index (all words, with normalised search key and per-word `wordMinTier`) |
| `data/dicts/base.dat` | All word records (display headword + per-meaning tier ranges) |
| `data/dicts/base.meta` | `name=Base (WordNet)`, `mode=additive`, `floor=safe`, `format=4` |

Total size is typically around 10 MB, well under the 14.4 MB LittleFS budget.
The script warns if the total exceeds 14 MB; lower `DICT_SENSE_CAP` (default 8)
or trim the word lists to recover space.

**Purge stale files from `data/dicts/` whenever you change the file set.** The
LittleFS image is generated from the whole directory; leftover files from prior
builds can push the image over the partition limit.

### Verify

```sh
python tools/verify_dict.py
```

Checks magic bytes, format version (`== 4`), key sort order, `wordMinTier`
consistency, total file size, spot-checks for common words, and simulated
visible-word counts per tier.

### Flash

```sh
pio run -t uploadfs
```

Flashes the contents of `data/` (including `data/dicts/`) into the LittleFS
partition. Firmware and filesystem flashes are independent; reflash only what
changed.

---

## Content classification lists

These files in `tools/` feed `build_dict.py` and control tier assignment for the
base WordNet dictionary. All are plain text (one entry per line, `#` comments);
the TSV files are tab-separated. Changes take effect on the next `build_dict.py`
run.

### Headword floor lists

Each list gates an entire headword — all of its meanings get at least that tier:

| File | Category | Floor |
|------|----------|-------|
| `tools/words_offensive.txt` | Strong profanity and slurs | Full |
| `tools/words_adult.txt` | Explicit sexual content (not slurs) | Teen |
| `tools/words_mild.txt` | Mild swears and toilet humour | Mild |
| *(unlisted)* | Everything else | Safe |

Words in `words_offensive.txt` are only visible at Full. Words deliberately
omitted (e.g. `cock`, `ass`) have their rude senses gated via gloss content or
`sense_labels.tsv`, allowing their innocent senses to remain at Safe.

### `core_harmful.txt` — gloss gating

A subset of the offensive and adult lists: tokens that are unacceptable **inside
a definition gloss** shown to a young child. If a WordNet definition or example
contains one of these tokens, that specific meaning's `minTier` is raised to Teen
(or Full if the token is itself offensive). Other meanings of the same word are
unaffected.

Note: the tokens `retard`, `retards`, `retarded` are in `core_harmful.txt` to
gate the headword floor, but are deliberately excluded from the gloss-gating pass
so that innocent medical or scientific definitions that reference the words are
not unnecessarily raised.

### `sense_labels.tsv` — per-sense tier overrides

For homograph words that have both an innocent sense and a rude sense,
`sense_labels.tsv` raises the `minTier` of the rude sense without touching the
innocent sense. Each row: `word TAB definition-prefix TAB tier`.

Example: the anatomical sense of *cock* is labelled Teen while its bird sense
remains at Safe. The headword is deliberately absent from the category lists so
the safe sense is not blocked.

### `overrides.tsv` — sanitised replacement definitions

For words that are themselves innocent but whose WordNet gloss contains a
core-harmful token (e.g. *fart*, *puberty*, *cattle*), `overrides.tsv` appends a
sanitised sibling meaning alongside the original. Each row:
`word TAB definition-prefix TAB sanitised-def [TAB sanitised-example]`.

The sanitised meaning gets `minTier = headword_floor(word)` and
`maxTier = originalMinTier - 1`, so it is only shown at lower tiers. The original
WordNet gloss remains in the data with its higher `minTier` and is shown at Teen
and above. Both meanings coexist in `base.dat`.

---

## More → Dictionaries settings UI

Open via **More → Dictionaries**.

### Source list

Each row in the source list represents one loaded dictionary and shows:

| Control | Labels | Action |
|---------|--------|--------|
| Name | (display name from `.meta`) | Read-only label |
| Enable pill | `On` / `Off` | Tap to toggle; disabled sources are excluded from the merged view |
| Priority buttons | `Up` / `Dn` | Swap priority with the neighbour row; `Up` at position 0 and `Dn` at the last position are greyed out |
| Mode pill | `Add` / `Ovr` | Tap to cycle: `Add` = additive, `Ovr` = override |
| Floor pill | `Safe` / `Mild` / `Teen` / `Full` | Tap to cycle the per-dictionary tier floor |

**Priority order:** position 0 (top) is highest priority. The merge walk proceeds
top → bottom; an `Ovr` source stops lower sources from contributing meanings to
any key it contains.

All changes persist to NVS immediately and rebuild the merged view. Settings
survive reboots. Per-source NVS keys are `s_<name>`.

### Exclusions section

Below the source list, an `Exclusions` section lists all loaded `.excl` files
with an `On`/`Off` toggle. Enabling or disabling a file rebuilds the merged view
immediately and persists the toggle to NVS.

---

## Keyboard — additional characters

The keyboard has four rows. The fourth row provides keys for characters used in
multi-word and hyphenated headwords:

| Key | Width | Notes |
|-----|-------|-------|
| Space | wide (150 px) | Appends a space to the query |
| `-` (hyphen) | narrow | Appends a hyphen |
| `'` (apostrophe) | narrow | Appends an apostrophe |

Because search-key normalisation drops apostrophes and collapses spaces, the
device finds the same result whether or not you type them. Typing `achilles heel`
and `achilles' heel` both match `Achilles' heel`. The `-`/`'` keys are never
dimmed by the dead-key logic (only `a`–`z` letter keys dim when they cannot
extend the current prefix).

---

## Content filtering disclaimer

**Filtering is best-effort; parental discretion is advised.** The category lists
are maintained by hand and cannot guarantee every sensitive term is covered. WordNet
definitions are used as-is except where explicit overrides have been applied. The
tier system is a good-faith effort to make the content age-appropriate but it is
not a substitute for supervision.
