# Supplementary dictionaries — design

**Date:** 2026-06-08
**Status:** Approved design (pending spec review)
**Device:** Freenove ESP32-S3 FNK0104 — 16 MB flash, 8 MB PSRAM, LovyanGFX, FT6336U touch, SD slot.

## Problem / goal

The device has one built-in dictionary (WordNet, format v3). The user wants to add
**swappable supplementary dictionaries** — children's dictionaries, mythology, bestiary,
people, places — moving toward an encyclopedia. Each can be toggled on/off, **reordered
on-device** by priority, marked **additive** (combine definitions) or **overriding**
(replace lower-rank entries), carry the same tier/PIN safety settings, and be loaded from
flash (LittleFS) and/or micro-SD. Plus user-customisable **runtime exclusion lists**, and
a first test dictionary (**Greek mythology**).

## Locked decisions (from brainstorming)

1. **Merge rule: per-word, mode-driven.** Walk enabled dicts high→low priority; an
   *additive* dict contributes its meanings and lets lower dicts keep adding; an
   *overriding* dict that contains the key contributes and **stops lower dicts for that
   key**. Words a dict lacks still come from lower dicts.
2. **Headwords: full (display + search key).** Store a display form ("Mount Olympus",
   "Achilles' heel") plus a normalised search key (lowercase, accents stripped,
   spaces/hyphens kept). Search is case-insensitive on the key; merge/override match on
   the key. Keyboard gains `-`, space, `'`.
3. **Tiers: per-dict floor + per-entry.** Each dict has a *minimum tier* setting (whole
   dict hidden below it) AND entries carry per-meaning tier bytes. Effective tier =
   `max(dict floor, entry minTier)`. Global active-tier + PIN gate everything.
4. **Exclusions: runtime drop-in files.** Live in the dicts folder; each has an action
   (hide / gate-to-tier) and scope (all / named dicts); toggleable on-device.
5. **Engine is runtime** (on-device reorder forces this; build-time merge rejected).

## Architecture

A runtime **multi-source dictionary engine** replacing the single-source `Dict` module.
The engine owns: discovery, per-source settings, the merged lookup/search/browse, tier
filtering, and exclusions. The UI and the rest of `main.cpp` talk to it through the
existing surface (`dictCount`, `dictTerm`, `dictGet`, `dictFind`, `dictLowerBound`,
`dictSetTier`, …) which now operate over the **merged, ordered, filtered** view.

### Storage layout & format v4

A **`/dicts/` folder** on LittleFS and SD. Each dictionary is three files:

- `<name>.idx` — `"DIDX"` | u32 version=**4** | u32 count | per entry
  `[u32 dataOffset][u8 wordMinTier][searchkey bytes][0x00]`. searchkey normalised
  (lowercase; accents → ASCII; spaces and `-` kept; `'` dropped), **sorted ascending**.
- `<name>.dat` — per entry at dataOffset: `[u8 dispLen][display headword]` then the v3
  meaning block: `[u8 nMeanings]` × `[u8 tierRange][u8 posCode][u16 defLen][def][u16 exLen][example]`.
  `tierRange = minTier | (maxTier<<2)`.
- `<name>.meta` — tiny `key=value` text: `name=`, `version=` (dataset stamp),
  `mode=additive|override`, `floor=safe|mild|teen|full`, `format=4`.

The base WordNet dict migrates into `/dicts/base.{idx,dat,meta}` (display == key for its
a-z words; `mode=override`-irrelevant since lowest priority; `floor=safe`). `build_dict.py`
emits v4. SD discovery: a dict on SD with the same `name` as a flash dict **shadows** it
(SD wins); new names on SD are **added**.

### Engine internals

- **Discovery (boot):** scan `/dicts/` on LittleFS then SD for `*.meta`; for each, confirm
  matching `.idx`/`.dat`; register a `Source{name, fs, idxBuf(PSRAM), entries[], meta,
  settings}`. Settings (`enabled, priority, mode, floor`) load from NVS keyed by name,
  defaulting to meta on first sight.
- **Index residency:** each enabled source's `.idx` is read into PSRAM and parsed to
  `{searchkey ptr, dataOffset, wordMinTier}` arrays (as today, per source). `.dat` stays
  on its filesystem, read on demand.
- **Active view:** maintain a merged, tier-filtered, deduped **key list** across enabled
  sources for the active tier (rebuilt on tier change / settings change). Browse and
  prefix-search/suggestions run over this. Dedup by key; the merged entry remembers which
  sources hold the key (for lookup).
- **Lookup (`dictGet` of a key):** iterate sources high→low priority; for each enabled
  source containing the key whose (dict floor, entry tier) passes the active tier, read its
  `.dat` entry, take display form from the first contributing source, append its visible
  meanings; if that source's mode is *override*, **stop**. Apply exclusions (below).
- **Exclusions:** `/dicts/exclude/*.excl` — header lines `action: hide|gate:<tier>` and
  `scope: all|name[,name]`, then one key per line. Loaded into a runtime map; during the
  active-view build and lookup, a key in an in-scope exclusion is hidden or has its
  effective tier raised. Toggleable per file in settings (NVS).
- **Memory:** base idx ~0.9 MB; supplementary indexes are small (mythology ≪ 1 MB). All in
  PSRAM (8 MB). Merged key view is pointers/ints. Well within budget.

### Settings UI (More → "Dictionaries")

- A scrollable, reorderable list. Each row: **name**, **enable** checkbox, **▲ / ▼**
  chevrons (swap priority with neighbour), **mode** toggle (Add ↔ Override), **floor**
  picker (Safe/Mild/Teen/Full). An **Exclusions** subsection lists `.excl` files with
  enable toggles. All changes persist to NVS and rebuild the active view.
- Reuses the existing drag-scroll list and tap patterns; chevrons are tap targets.

### Keyboard / charset

Restore a 4th keyboard row: **space**, **hyphen `-`**, **apostrophe `'`** (plus the
existing backspace on row 3). Search is case-insensitive on the normalised key, so **no
shift/caps key** — the user types lowercase and `Zeus`/`Mount Olympus` match; the
definition screen shows the proper **display** form. The apostrophe key is for input
fidelity (`Achilles'`) but is **ignored in matching** (normalisation drops `'`), so a word
matches whether or not the apostrophe is typed. The red X clear button stays.
`buildKeyboard` and hit-testing extend for the new keys; preview/search already key off
the normalised query.

### Greek-mythology test dictionary

`tools/sources/mythology.json` — a hand-curated set (~150 entries: Olympians, heroes,
monsters, places; e.g. Zeus, Hera, Athena, Medusa, Minotaur, Achilles, Mount Olympus),
each with display name, one or more concise **original-wording** definitions (avoid
copying licensed text), POS = "noun"/"name", and an optional per-entry tier. Built by an
extended generator into `mythology.{idx,dat,meta}` with `mode=additive`, `floor=safe`;
mature myths gated per-entry. Delivered to `/dicts/` via SD (no reflash) or uploadfs.

## Documentation (in scope)

Update `README.md` and add a `docs/` page covering: the `/dicts/` layout, format v4, the
`.meta`/`.excl` file formats, how to author and install a supplementary dictionary, the
core classification lists (`words_offensive/adult/mild.txt`, `core_harmful.txt`,
`sense_labels.tsv`, `overrides.tsv`) and how they extend, and the Dictionaries settings UI.

## Build / delivery phases (one spec, staged plan)

1. **Format v4 + base migration** — generator emits v4; firmware reads v4; base dict in
   `/dicts/`. (No behaviour change yet; single source.)
2. **Engine** — multi-source discovery, NVS settings, merged lookup/search/browse, tier
   floor, mode merge.
3. **Keyboard/charset** — `-`, space, `'`; normalised search.
4. **Settings UI** — Dictionaries list, chevrons, toggles, floor.
5. **Mythology content** — curated JSON + build + on-device test.
6. **Exclusions** — `.excl` loading, application, UI toggles.
7. **Docs**.

## Risks / mitigations

- **Flash/partition vs OTA:** the publishing research flags that an OTA-capable partition
  table should be decided before a public v1.0; supplementary dicts are best delivered via
  SD / LittleFS image, not OTA. Noted for the roadmap; this spec keeps the current
  partition and the SD-overlay path.
- **Merged-view cost at scale:** if many large dicts are enabled, the merged key view and
  browse could grow; bound by tier filtering and the small size of supplementary dicts;
  revisit if a future dict rivals the base in size.
- **Search-key collisions across very different display forms** (e.g. accents) — acceptable;
  display form disambiguates on screen.
- **Backwards compat:** v4 is a format bump; the firmware rejects non-v4 with a clear
  status and falls back to the embedded set. All data is regenerated.
