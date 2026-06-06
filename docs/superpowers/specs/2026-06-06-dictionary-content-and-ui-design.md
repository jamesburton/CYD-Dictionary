# Dictionary content & definition-UI redesign

**Date:** 2026-06-06
**Status:** Approved design (pending spec review)
**Device:** Freenove ESP32-S3 Display FNK0104 — 16 MB flash, 8 MB PSRAM, LovyanGFX, FT6336U capacitive touch.

## Problem

The current build embeds the full Wordset corpus (64,645 words) with **one meaning per
word, taken as Wordset's first listed meaning**. That meaning is frequently the wrong or
obscure sense — `toy`→a dog breed, `blue`→depression rather than the colour. The corpus
also contains profanity and adult content unsuitable as a general baseline, and entirely
unsuitable for the user's 8-year-old son.

## Goals

1. **Multiple meanings per word**, ordered by frequency, excluding the obscure long tail.
2. **Scrollable definition screen** (drag to scroll) so all common meanings are readable.
3. A **better baseline ("Everyone")** dictionary with profanity/offensive entries removed.
4. A **child-friendly ("Kids")** dictionary, broad but safety-filtered, for the son.
5. **Runtime tier switch** in the More menu, with a **PIN lock** so Kids mode stays put.

Non-goals: perfect word-sense frequency ranking (WordNet's is good-enough and iterable);
audio; online lookup.

## Data source & pipeline

**Source: WordNet** (via `nltk`), with **`wordfreq`** for word-level frequency.

WordNet provides, per lemma, multiple synsets already roughly frequency-ranked, each with a
definition (gloss), part of speech, and (sometimes) example sentences. This directly fixes
the bad-default problem: `blue`→colour, `toy`→plaything, `mouse`→rodent all rank first.

**Known limitation (accepted):** WordNet's sense frequency comes from the SemCor tagged
corpus, which is skewed (`bank`→"sloping land" edges out "financial institution"; `run`
ranks "baseball run" high). Because we now show *several* meanings, an imperfect #1 is
minor — the common sense is still present. Ordering is a later refinement.

**Per-word extraction** (in `tools/build_dict.py`, rewritten):
- Headwords: WordNet single-word lemmas matching `^[a-z]+$` (~63,700).
- Meanings: synsets for the lemma, ordered by `lemma.count()` descending, then native
  WordNet order; **capped at N** (N tuned so the shared `dict.dat` fits flash — expect
  ~6–8). The cap is the size lever; word coverage is preserved.
- Each meaning: `{posCode, definition (gloss), example}`. Examples are included for
  **every sense that has one** (per user choice).
- Definitions truncated to a sane max (≈240 chars) to bound record size.

**Filtering (two blocklists, both editable text files under `tools/`):**
- `tools/blocklist_profanity.txt` — profanity/slurs (seeded from the public LDNOOBW
  English list). Applied to the **Everyone** tier (removes the headword entirely).
- `tools/blocklist_sensitive.txt` — broader child-sensitive terms (sexual, drugs, graphic
  violence, slurs, explicit anatomy), seeded curated. Applied **in addition** for the
  **Kids** tier.
- A word is removed if its headword matches a blocklist. (Meaning-level filtering is a
  later refinement; headword removal is the v1 policy.)

## Shared-data storage model

The Kids tier is a **strict subset** of Everyone with **identical definitions**. So the
word data is stored **once**; tiers differ only by which headwords they expose.

The generator emits three files into `data/` (flashed via `pio run -t uploadfs`):

| File             | Contents                                                            |
|------------------|--------------------------------------------------------------------|
| `dict.dat`       | All Everyone-tier word records (multi-meaning). **Shared.**         |
| `dict.idx`       | Everyone index — every word, with its `dict.dat` offset.           |
| `dict_kids.idx`  | Kids index — subset of words, pointing into the **same** `dict.dat`.|

Switching tiers reloads only the ~0.8 MB index; `dict.dat` stays open. Estimated sizes:
`dict.dat` ~12 MB, `dict.idx` ~0.9 MB, `dict_kids.idx` ~0.8 MB → ~13.7 MB total.

### On-disk format (version 2)

All integers little-endian.

**`*.idx`** (unchanged shape, version bumped):
```
magic   "DIDX"
u32     version (2)
u32     count
count × [ u32 dataOffset ][ term bytes ][ 0x00 ]      # terms lowercase a-z, sorted asc
```

**`dict.dat`** (new, multi-meaning, 1-byte POS code):
```
per entry at dataOffset:
  u8   nMeanings
  nMeanings × [ u8 posCode ][ u16 defLen ][ def ][ u16 exLen ][ example ]
```
`posCode`: 0 noun, 1 verb, 2 adjective, 3 adverb, 4 other. Meanings are stored in flat
frequency order; the device groups them by POS for display.

## Partition layout (16 MB)

Shrink the app partition (only ~0.5 MB used) to make room for the larger filesystem.
`partitions.csv`:
```
# Name,   Type, SubType, Offset,   Size
nvs,      data, nvs,     0x9000,   0x5000,
factory,  app,  factory, 0x10000,  0x180000,   # 1.5 MB app
spiffs,   data, spiffs,  0x190000, 0xE70000,   # ~14.4 MB LittleFS data
```

## Firmware changes

### `Dict` module (`src/Dict.{h,cpp}`)
- `DictEntry` gains a meanings list:
  ```cpp
  struct Meaning { uint8_t posCode; String def; String example; };
  struct DictEntry { String term; std::vector<Meaning> meanings; };
  ```
- Parse format v2 in `dictGet` (loop `nMeanings`). Reject version != 2 with a clear status.
- **Tier selection:** `dictBegin(tier)` chooses the index file (`dict.idx` /
  `dict_kids.idx`) over the shared `dict.dat`. `dictSetTier(tier)` frees the current index
  PSRAM and reloads the other (dat handle and source FS unchanged).
- Source priority is unchanged: SD override → LittleFS (built-in) → embedded fallback. SD
  override applies to whichever tier file is requested; embedded fallback remains single-
  meaning (one `Meaning`).
- `posName(posCode)` → "noun"/"verb"/… for display.

### Definition screen (`src/main.cpp`)
- Render **grouped by part of speech** (layout B): a POS header (NOUN / VERB / …) then its
  senses numbered within the group, each with definition (wrapped) and example(s) in quotes.
  Grouping preserves first-seen POS order from the flat meaning list.
- **Fixed header** (Back / headword / ★) ; only the meanings area scrolls.
- **Drag-to-scroll:** a dedicated handler reads continuous touch on this screen. Track press
  start `(y0, scroll0)`; while held, `scrollY = clamp(scroll0 + (y0 - yNow), 0, maxScroll)`
  and redraw the content viewport (filled background + meanings offset by `-scrollY`, clipped
  to the viewport). A movement threshold (~6 px) distinguishes a **drag** (scroll) from a
  **tap** (Back / ★). A thin scrollbar + thumb indicates position; hidden when content fits.
  - Flicker control: render the scrolling viewport via a PSRAM sprite (320 × content-height,
    or a viewport-height sprite blitted per frame) for smooth scrolling. Plenty of PSRAM
    (~8 MB free). If a sprite proves unnecessary, direct clipped redraw is the fallback.
- Search preview, suggestion chips, and list rows use `meanings[0]` (primary sense).

### More screen — tier toggle + PIN lock (`src/main.cpp`, NVS)
- New row: **`Dictionary: [ Everyone | Kids ]`** showing the active tier.
- Tapping toggles tier: Everyone→Kids is always allowed; Kids→Everyone requires the PIN
  **if a lock is set**. On switch, call `dictSetTier`, persist the choice (NVS `tier`), and
  redraw.
- **PIN:** stored in NVS (`pin`, 4 digits). A "Set/Change PIN" action shows a simple numeric
  keypad to set it; an empty PIN means "no lock" (free switching). When locked and leaving
  Kids mode, a keypad prompts for the PIN; wrong PIN cancels the switch.
- Persisted NVS keys: `tier` (0/1), `pin` (string, "" = unlocked). Existing `favs`, `hist`,
  `boot`, `touchcal` unchanged.

## Build & flash workflow

```sh
pip install nltk wordfreq            # one-time; build_dict.py downloads WordNet (+omw) on first run
python tools/build_dict.py           # writes data/dict.dat, data/dict.idx, data/dict_kids.idx
pio run -t upload                    # firmware
pio run -t uploadfs                  # dictionary into LittleFS
```
The Wordset corpus is no longer the source (WordNet replaces it); `tools/wordset/` and the
old single-meaning `build_dict.py` logic are removed. The embedded `WordData.h` fallback stays.

## Testing / acceptance

- `blue`, `toy`, `mouse` show the common sense first; multiple meanings listed.
- Definition screen scrolls smoothly by dragging; header stays fixed; scrollbar reflects
  position; short entries don't scroll.
- More shows the active tier; toggling reloads the word set (Browse count changes).
- With a PIN set, leaving Kids mode requires it; wrong PIN cancels.
- Profanity absent from Everyone; sensitive terms additionally absent from Kids.
- Boot log: `[dict] ready: Flash (Everyone): N words` (or Kids).
- `dict.dat` + both indexes fit the LittleFS partition with headroom.

## Risks / mitigations

- **Size overrun:** cap meanings (N) and/or definition length to keep `dict.dat` within the
  ~14 MB partition; coverage is preserved by trimming senses, not words.
- **Filtering completeness:** blocklists can't be perfect; they're editable text files and
  the Kids list errs toward removal. Documented as best-effort.
- **Scroll flicker:** mitigated with a PSRAM sprite; direct clipped redraw as fallback.
- **WordNet sense order:** imperfect but acceptable given multi-meaning display; iterable.
