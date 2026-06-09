# CYD Dictionary

An offline touchscreen English dictionary for the **Freenove ESP32-S3 Display
FNK0104** (variant AB). No cloud, no SD card required — the full dictionary lives
in the device's own flash.

**Hardware:** 2.8" 240×320 IPS display (ILI9341), FT6336U capacitive touch,
ESP32-S3-WROOM-1 N16R8 (16 MB flash, 8 MB octal PSRAM), microSD slot (SDMMC).

**Toolchain:** PlatformIO + Arduino framework + LovyanGFX 1.2.7.

**Dictionary source:** WordNet (via NLTK), ~63,700 single-word lemmas, up to 8
meanings per word, frequency-ordered.

---

## Features

### Search-first UI

The default screen is a keyboard with instant feedback:

- **Key popups** — an enlarged bubble appears above each pressed letter.
- **Tappable suggestion chips** — up to 6 prefix-matched words shown as pills
  below the search field; tap one to jump straight to its definition.
- **Live preview** — the top match's headword, part of speech, and first
  definition are shown while you type.
- **Dimmed dead-end keys** — letters that cannot extend the current query are
  greyed out and non-tappable.
- **Red clear button** — one tap clears the whole query.

A persistent **bottom tab bar** gives access to four sections:

| Tab | Contents |
|-----|----------|
| Search | Keyboard + suggestions + preview (default) |
| Browse | Scrollable A–Z word list |
| Saved | Your starred (favourite) words |
| More | Tier selector, word of the day, random word, recent words, touch recalibration, PIN management |

### Definition screen

- **Multiple meanings** grouped by part of speech (NOUN / VERB / ADJECTIVE /
  ADVERB), numbered within each group, with WordNet example sentences in quotes.
- **Drag-to-scroll** — drag up/down to scroll through long entries; a movement
  threshold of 6 px distinguishes a drag from a tap on the Back or favourite
  buttons. Scrolling is rendered via a PSRAM sprite for flicker-free performance.
- **Scrollbar** — thin indicator on the right edge; hidden when content fits the
  screen without scrolling.
- **Fixed header** — Back button and favourite toggle stay in place while the
  meanings area scrolls.

### Favourites and history

- Tap **+ fav** (top-right of any definition) to star a word; tap again to
  unstar. Starred words appear in the **Saved** tab.
- The last 12 looked-up words are kept as **Recent words**, accessible from
  **More → Recent words**.
- Both lists survive reboots (stored in NVS).

### Word of the day

Shown as a card at the top of the **More** screen. Rotates daily based on a
boot counter (tap the card to open its definition).

### Touch calibration

Runs automatically on first boot (corner-tap routine). Re-run any time from
**More → Recalibrate touch**. The calibration transform is stored in NVS.

---

## Content tiers

Words are filtered into four tiers, switchable in the **More** screen:

| Tier | Shows |
|------|-------|
| Safe | Only meanings whose full definition is suitable for all ages |
| Mild | Adds slightly rude / toilet-humour content |
| Teen | Adds sexual / explicit content (excludes only strong profanity and slurs) |
| Full | Everything — the complete WordNet set |

### How tier filtering works

**Filtering is per-meaning, not per-word.** Every meaning stored in the
dictionary carries a `[minTier, maxTier]` range. A meaning is shown only when
`minTier ≤ activeTier ≤ maxTier`. A word is hidden entirely only when none of
its meanings are visible at the active tier.

This means a word like *cock* (rooster) stays visible at Safe while the
anatomical sense is gated to Teen. Likewise, a word like *puberty* shows a
child-appropriate definition at Safe/Mild and the full WordNet gloss only at
Teen and above.

The tier ranges on each meaning are set by four complementary mechanisms,
applied during the build:

**1. Headword floor** (`words_offensive.txt`, `words_adult.txt`, `words_mild.txt`)

The headword's category determines the floor for all its meanings:

| File | Category | Floor |
|------|----------|-------|
| `tools/words_offensive.txt` | Strong profanity and slurs | Full |
| `tools/words_adult.txt` | Sexual / explicit content | Teen |
| `tools/words_mild.txt` | Naughty / slightly rude words | Mild |
| *(not listed)* | Everything else | Safe |

A word that appears in `words_offensive.txt` has every meaning gated to Full
— it only appears in the Full tier. Words in `words_adult.txt` floor at Teen,
and so on.

**2. Gloss gating** (`core_harmful.txt`)

A small curated set of tokens (`tools/core_harmful.txt` — slurs, explicit
anatomy, sexual acts) that are unacceptable even *inside a definition* read by
a young child. If a WordNet definition or example contains one of these tokens,
that specific meaning's `minTier` is raised to Teen (or Full if the token is
itself offensive). This gating is applied only to the individual meaning whose
text contains the term — other meanings of the same word are unaffected.

**3. Per-sense labels** (`tools/sense_labels.tsv`)

Homograph words whose innocent sense must be kept at Safe while a rude sense is
gated higher. Each row names the word, a definition prefix identifying the
sense, and the target tier. For example, the "penis" sense of *cock* and *tit*
are labelled Teen while their bird/animal senses remain Safe. The headword is
deliberately *omitted* from the category lists so the safe sense is not blocked.

**4. Sanitised definitions** (`tools/overrides.tsv`)

For words whose WordNet gloss contains a core-harmful term but which are
themselves innocent (e.g. *fart*, *bowel*, *puberty*, *cattle*), the build
appends a sanitised sibling meaning alongside the original. The sanitised
version is visible at low tiers (`maxTier = labelledTier − 1`), while the
explicit WordNet text is only shown at Teen and above. Both meanings coexist in
`dict.dat`; the active tier determines which one is returned.

### PIN lock

The More screen shows the current tier and a **locked / unlocked** indicator.
Switching to a *more permissive* tier (e.g. Safe → Teen) requires a PIN if one
is set. Switching to a *more restrictive* tier is always free. Set or clear the
PIN via **More → Set / Change PIN**; an empty entry clears the lock.

**Filtering is best-effort.** The category lists are editable plain-text files
(one word per line, `#` comments); updates take effect on the next
`build_dict.py` run.

---

## Build and flash

### Prerequisites

```sh
pip install nltk wordfreq
```

WordNet data is downloaded automatically on the first run of `build_dict.py`
(requires internet access for that one run only).

### 1 — Generate the dictionary files

```sh
python tools/build_dict.py
```

Outputs into `data/dicts/` (PlatformIO's filesystem-image source directory):

| File | Contents |
|------|----------|
| `data/dicts/base.idx` | Index (all words, normalised search key, per-word minimum tier) |
| `data/dicts/base.dat` | All word records with display headword and per-meaning tier ranges |
| `data/dicts/base.meta` | Dictionary metadata (`name`, `mode`, `floor`, `format=4`) |

The `data/` directory is git-ignored. Total size is typically around 10 MB,
well under the 14.4 MB LittleFS budget; the script warns if this is exceeded and
suggests lowering `DICT_SENSE_CAP` (default 8).

**Purge stale files from `data/dicts/` if you change the file set** before
flashing — leftover files inflate the LittleFS image and can overflow the partition.

### 2 — Optional: verify the output

```sh
python tools/verify_dict.py
```

Sanity-checks the generated files: magic bytes, format version, term sort order,
`wordMinTier` consistency, total size, spot-checks for common words, and a
simulated visible-word count per tier.

### 3 — Flash the firmware

```sh
pio run -t upload
```

Port is `COM5` (see `platformio.ini`). Change `upload_port` if your device is on
a different port.

### 4 — Flash the dictionary into LittleFS

```sh
pio run -t uploadfs
```

This flashes the contents of `data/` into the LittleFS partition. Both steps are
independent; reflash only what changed.

### Build-only (no flash)

```sh
pio run
```

### Serial note

`platformio.ini` sets `ARDUINO_USB_CDC_ON_BOOT=0`, so the Arduino `Serial` object
is on **UART0** (physical TX/RX pins), not the USB port. Only ROM/ESP-IDF output
and `esp_rom_printf` calls reach the USB console — including the boot status line:

```
[dict] ready: Flash: 63714 words (Full)
```

---

## How the dictionary is built

### Data source

WordNet is accessed via NLTK's Python interface. For each single-word `[a-z]+`
lemma, the build script collects synsets ordered by `lemma.count()` descending
(WordNet's SemCor-derived frequency), then native WordNet order as a tiebreaker.
This places the most common sense first: `blue` → colour, `toy` → plaything,
`mouse` → rodent. Up to `DICT_SENSE_CAP` (default 8) meanings are kept per word.

Definitions are capped at 240 characters; examples at 160 characters.

### On-disk format (format v4)

Dictionaries use **format v4** — each dictionary is three files: `<name>.idx`
(index with normalised search key and per-word minimum tier), `<name>.dat` (word
data with display headword and per-meaning tier ranges), and `<name>.meta`
(plain-text metadata). Full byte-layout details are in
[`docs/dictionaries.md`](docs/dictionaries.md).

### Source discovery at boot

The engine scans `/dicts/` on LittleFS (flash) first, then SD. Each `*.meta`
file registers a source; an SD dictionary with the same filename stem as a flash
dictionary **shadows** (replaces) it. New stems on SD are added alongside flash
dictionaries. If no sources load, the firmware falls back to an embedded ~90-word
list baked into `src/WordData.h`.

The active-source count and word count are shown on the **More** screen and
printed at boot.

---

## Supplementary dictionaries

The dictionary engine supports multiple runtime sources loaded from `/dicts/` on
LittleFS and/or a FAT32 SD card. Each source is an independently toggleable,
reorderable dictionary with its own tier floor and merge mode.

**Key capabilities:**

- **Multi-source, priority-ordered** — sources are listed highest to lowest
  priority in the Dictionaries settings screen. Tap **Up** / **Dn** to reorder.
- **Additive or override merge** — an *additive* source contributes its meanings
  alongside lower-priority sources for the same word; an *override* source stops
  lower sources for any word it contains.
- **Per-dictionary tier floor** — a dictionary can be hidden entirely below a
  chosen tier (e.g. a mythology dict set to `floor=safe` is always visible; an
  explicit-content supplement could be set to `floor=teen`).
- **Runtime exclusion files** — `.excl` files in `/dicts/exclude/` can hide or
  gate individual words by key, scoped to all or named dictionaries.
- **Greek mythology** — a curated set of ~150 entries (Olympians, heroes,
  monsters, places) is included as a sample supplementary dictionary built from
  `tools/sources/mythology.json`.

**Build a supplementary dictionary:**

```sh
python tools/build_supp.py tools/sources/mythology.json mythology --mode additive --floor safe
pio run -t uploadfs
```

Or copy the three output files (`mythology.idx`, `mythology.dat`, `mythology.meta`)
to `/dicts/` on a FAT32 SD card — no reflash needed.

See [`docs/dictionaries.md`](docs/dictionaries.md) for the full format
specification, JSON schema, classification lists, and UI reference.

---

## Repo layout

| Path | Contents |
|------|----------|
| `src/` | Firmware (main.cpp, Dict.h/cpp, DisplayConfig.hpp, LGFX_Setup.hpp, WordData.h) |
| `tools/` | Data pipeline, word lists, and supplementary sources (see below) |
| `data/` | Generated dictionary files — **git-ignored**, produced by the build scripts |
| `docs/` | Documentation; design specs and implementation plans under `docs/superpowers/` |
| `platformio.ini` | PlatformIO build configuration |
| `partitions.csv` | Custom partition table (1.5 MB app, ~14.4 MB LittleFS) |

### Tools directory

| File | Role |
|------|------|
| `tools/build_dict.py` | Base dictionary generator — reads WordNet + word lists, writes `data/dicts/base.*` |
| `tools/build_supp.py` | Supplementary dictionary builder — converts a JSON source to `data/dicts/<name>.*` |
| `tools/normalize.py` | Shared search-key normalisation used by both builders and the verifier |
| `tools/verify_dict.py` | Post-build verifier — checks format, consistency, sizes |
| `tools/sources/mythology.json` | Curated Greek-mythology source (~150 entries) |
| `tools/sample-exclusions/extra-rude.excl` | Sample exclusion file (copy to `data/dicts/exclude/`) |
| `tools/words_offensive.txt` | Strong profanity + slurs → headword floor Full |
| `tools/words_adult.txt` | Sexual / explicit content → headword floor Teen |
| `tools/words_mild.txt` | Naughty / slightly rude words → headword floor Mild |
| `tools/core_harmful.txt` | Tokens unacceptable inside a definition gloss (drives per-meaning gating) |
| `tools/sense_labels.tsv` | Per-sense tier overrides for homograph rude senses |
| `tools/overrides.tsv` | Sanitised replacement definitions for sensitive-gloss words |

All word-list files are plain text (one entry per line, `#` comments); the TSV
files have tab-separated columns. All are editable; changes take effect on the
next `build_dict.py` run.

### Partition layout

```
# Name,   Type, SubType, Offset,   Size
nvs,      data, nvs,     0x9000,   0x5000
factory,  app,  factory, 0x10000,  0x180000   # 1.5 MB app
spiffs,   data, spiffs,  0x190000, 0xE70000   # ~14.4 MB LittleFS
```

The `spiffs` label is required for `LittleFS.begin()` and `pio run -t uploadfs`
to find the partition.

---

## Hardware pins

See `src/DisplayConfig.hpp` for the full pin table. Summary:

| Peripheral | Interface | Key pins |
|------------|-----------|----------|
| ILI9341 display | HSPI (SPI2) | MOSI 11, SCLK 12, CS 10, DC 46, BL 45 |
| FT6336U touch | I2C (port 0) | SDA 16, SCL 15, INT 17, RST 18 |
| microSD | SDMMC 4-bit | CLK 38, CMD 40, D0–D3 39/41/48/47 |

Pin assignments are taken verbatim from Freenove's own FNK0104AB example sketches.
