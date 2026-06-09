# Supplementary Dictionaries Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the single WordNet dictionary into a runtime multi-source engine — toggleable, reorderable, additive/overriding supplementary dictionaries (mythology, people, places…) plus runtime exclusion lists — all loaded from `/dicts/` on LittleFS and/or SD, with per-dict tier/PIN safety and a settings UI.

**Architecture:** New on-disk **format v4** (display headword + normalised search key, per-meaning tier bytes). A rewritten `Dict` engine loads each dictionary's index into PSRAM, keeps a merged/tier-filtered/deduped key view across enabled sources (priority-ordered), and resolves a lookup by walking sources high→low priority (additive appends, overriding stops). Runtime `.excl` files reclassify/hide keys. A More→Dictionaries screen toggles/reorders/configures sources; the keyboard gains `-`, space, `'`.

**Tech Stack:** PlatformIO (espressif32@6.9.0), Arduino, LovyanGFX 1.2.7, ESP32-S3 (16MB flash / 8MB PSRAM), LittleFS, SD_MMC, Preferences (NVS); Python 3 + nltk + wordfreq for the generator.

**Spec:** `docs/superpowers/specs/2026-06-08-supplementary-dictionaries-design.md`

**Branch:** create `feature/supplementary-dictionaries` before Task 1 (`git checkout -b feature/supplementary-dictionaries`).

**Device:** Freenove FNK0104 on COM5. Build `~/.platformio/penv/Scripts/pio.exe run`; flash firmware `… -t upload`; flash data `… -t uploadfs`; boot log `python tools/boot_log.py`. **Subagents must NOT flash/serial — the controller runs those.** clang/IntelliSense errors (`'Arduino.h' not found`, `No member named 'color565'`) are false positives; only `pio run` is authoritative.

---

## File structure

| File | Responsibility |
|------|----------------|
| `tools/build_dict.py` | Generate the **base** dict in v4 into `data/dicts/base.{idx,dat,meta}` |
| `tools/build_supp.py` | NEW — build a supplementary dict from a JSON source into `<name>.{idx,dat,meta}` |
| `tools/normalize.py` | NEW — shared search-key normalisation (used by both builders + verifier) |
| `tools/verify_dict.py` | Verify v4 files (base + supplementary) |
| `tools/sources/mythology.json` | NEW — curated Greek-mythology entries |
| `src/Dict.h` / `src/Dict.cpp` | Multi-source engine (discovery, merge, tiers, exclusions, source settings) |
| `src/main.cpp` | Keyboard rows; More→Dictionaries settings screen; consumers use engine API |
| `partitions.csv` | unchanged (1.5MB app + 14.4MB LittleFS) |
| `docs/dictionaries.md` | NEW — authoring/installing dicts, formats, classification lists |

**On-device layout:** `uploadfs` flashes `data/` → device sees `/dicts/base.{idx,dat,meta}` (+ any bundled supplementary), and `/dicts/exclude/*.excl`. SD is scanned at `/dicts/` too.

---

## Format v4 (authoritative — both builders and firmware)

Tiers: `SAFE=0, MILD=1, TEEN=2, FULL=3` (higher = more permissive). Little-endian.

- **`<name>.idx`**: `"DIDX"` | u32 version=**4** | u32 count | per entry `[u32 dataOffset][u8 wordMinTier][searchkey bytes][0x00]`. searchkey sorted ascending.
- **`<name>.dat`**: per entry at dataOffset: `[u8 dispLen][display bytes]` then `[u8 nMeanings]` × `[u8 tierRange][u8 posCode][u16 defLen][def][u16 exLen][example]`, `tierRange = minTier | (maxTier<<2)`.
- **`<name>.meta`**: text `key=value` lines: `name=`, `version=`, `mode=additive|override`, `floor=safe|mild|teen|full`, `format=4`.

**Search-key normalisation** (identical in Python and C++): lowercase; strip diacritics to ASCII; keep `a-z`, `0-9`, space, `-`; drop `'`; collapse runs of spaces to one; trim. e.g. `"Achilles' heel"`→`"achilles heel"`, `"Mount Olympus"`→`"mount olympus"`.

---

## Phase 1 — Format v4 + base migration

### Task 1.1: Shared normaliser (Python)

**Files:** Create `tools/normalize.py`

- [ ] **Step 1: Write the test**

Create `tools/test_normalize.py`:
```python
from normalize import norm_key
def test_norm():
    assert norm_key("Achilles' heel") == "achilles heel"
    assert norm_key("Mount  Olympus") == "mount olympus"
    assert norm_key("café") == "cafe"
    assert norm_key("ZEUS") == "zeus"
    assert norm_key("well-known") == "well-known"
    print("ok")
test_norm()
```

- [ ] **Step 2: Run it, expect failure**

Run: `cd tools && python test_normalize.py`
Expected: `ModuleNotFoundError: No module named 'normalize'`.

- [ ] **Step 3: Implement**

Create `tools/normalize.py`:
```python
"""Search-key normalisation shared by the dictionary builders and verifier.
MUST match the C++ implementation in src/Dict.cpp (dictNormalizeKey)."""
import re
import unicodedata

_KEEP = re.compile(r"[^a-z0-9 \-]")
_WS = re.compile(r"\s+")


def norm_key(s: str) -> str:
    s = unicodedata.normalize("NFKD", s)
    s = "".join(c for c in s if not unicodedata.combining(c))  # strip accents
    s = s.lower().replace("'", "").replace("’", "")        # drop apostrophes
    s = _KEEP.sub(" ", s)                                       # other punct -> space
    s = _WS.sub(" ", s).strip()
    return s
```

- [ ] **Step 4: Run it, expect pass**

Run: `cd tools && python test_normalize.py`
Expected: `ok`.

- [ ] **Step 5: Commit**

```bash
git add tools/normalize.py tools/test_normalize.py
git commit -m "Add shared search-key normaliser"
```

### Task 1.2: build_dict.py → v4 base into /dicts/

**Files:** Modify `tools/build_dict.py`

- [ ] **Step 1: Change output paths and add meta**

In `tools/build_dict.py`, change the output directory and add a meta writer. Replace the `OUT_DIR`/`write_idx` area so it writes to `data/dicts/` with base name `base`:
```python
OUT_DIR = os.path.join(os.path.dirname(HERE), "data", "dicts")
VERSION = 4
```
Add near the other helpers:
```python
def write_meta(path, name, mode, floor):
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"name={name}\nversion=dict-2026.06\nmode={mode}\nfloor={floor}\nformat=4\n")
```

- [ ] **Step 2: Emit display + key in the records**

The base corpus keys are already lowercase `a-z`, so display == key. Change `write_idx` to the v4 entry layout and the `.dat` writer to prefix each entry with the display headword. Replace the index writer:
```python
def write_idx(path, words, offsets, wordmin):
    with open(path, "wb") as f:
        f.write(b"DIDX"); f.write(struct.pack("<II", VERSION, len(words)))
        for w in words:
            f.write(struct.pack("<I", offsets[w]))
            f.write(struct.pack("<B", wordmin[w]))
            f.write(norm_key(w).encode("ascii") + b"\x00")   # key
```
and in the `dict.dat` writer, before `nMeanings`, write the display form:
```python
            disp = w.encode("utf-8")[:255]
            dat.write(struct.pack("<B", len(disp)) + disp)   # display headword
            dat.write(struct.pack("<B", len(ms)))
            # ... existing per-meaning writes ...
```
Add `from normalize import norm_key` at the top. Keep all existing tier/sense_labels/overrides logic; `wordmin[w]` is the per-word min tier you already compute. Write `data/dicts/base.idx`, `base.dat`, and call `write_meta("data/dicts/base.meta", "Base (WordNet)", "additive", "safe")`. (Base is lowest priority by default; mode is irrelevant there but set additive.)

- [ ] **Step 3: Run the generator**

Run: `python tools/build_dict.py`
Expected: prints counts; writes `data/dicts/base.idx`, `base.dat`, `base.meta`. Total still ~10MB.

- [ ] **Step 4: Commit** (`data/` is git-ignored; commit scripts only)

```bash
git add tools/build_dict.py
git commit -m "build_dict: emit format v4 (display + normalised key) into data/dicts/base.*"
```

### Task 1.3: verify_dict.py → v4

**Files:** Modify `tools/verify_dict.py`

- [ ] **Step 1: Rewrite the parser for v4 and assert**

Replace the idx/entry readers so they parse v4 (`version==4`, `wordMinTier` byte, key string) and the dat entry (dispLen+display, then meanings with tierRange). Assert: version==4; keys sorted; `wordMinTier == min(meaning.minTier)`; each meaning `minTier<=maxTier`; display non-empty; `blue`/`toy`/`mouse` resolve (search by `norm_key`); `fuck` wordMinTier==FULL; total `data/dicts/base.*` ≤ 14MB. Use `from normalize import norm_key`. Point `DATA` at `data/dicts` and read `base.idx`/`base.dat`.

- [ ] **Step 2: Run it**

Run: `python tools/verify_dict.py`
Expected: prints the spot-checks and `OK ...`, exit 0.

- [ ] **Step 3: Commit**
```bash
git add tools/verify_dict.py
git commit -m "verify_dict: format v4"
```

### Task 1.4: C++ normaliser + v4 single-source read (no behaviour change yet)

**Files:** Modify `src/Dict.h`, `src/Dict.cpp`

- [ ] **Step 1: Add the normaliser and v4 parse to Dict.cpp**

Add to `src/Dict.cpp` (matches `tools/normalize.py`):
```cpp
// Normalise a query/headword to a search key: lowercase, strip accents to ASCII,
// keep a-z 0-9 space '-', drop apostrophes, collapse spaces. MUST match
// tools/normalize.py. ASCII-only accent fold (covers Latin-1 supplement).
void dictNormalizeKey(const char* in, char* out, size_t cap)
{
    size_t o = 0; bool lastSpace = true;
    for (const unsigned char* p = (const unsigned char*)in; *p && o + 1 < cap; ++p) {
        unsigned char c = *p;
        char m = 0;
        if (c >= 'A' && c <= 'Z') m = c - 'A' + 'a';
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) m = c;
        else if (c == '-') m = '-';
        else if (c == ' ' || (c >= 9 && c <= 13)) m = ' ';
        else if (c == '\'' || c == 0x92) continue;       // drop apostrophes
        else m = ' ';                                     // other punct/UTF-8 -> space
        if (m == ' ') { if (lastSpace) continue; lastSpace = true; }
        else lastSpace = false;
        out[o++] = m;
    }
    while (o > 0 && out[o-1] == ' ') --o;                  // trim trailing
    out[o] = 0;
}
```
Declare in `Dict.h`: `void dictNormalizeKey(const char* in, char* out, size_t cap);`

- [ ] **Step 2: Read v4 in the existing loader**

In `Dict.cpp`, change the index path to `/dicts/base.idx`, the dat path to `/dicts/base.dat`, the version check to `== 4`, parse the extra `wordMinTier` byte after `dataOffset` (already present logic from v3 used a per-word min byte — keep), and in `dictGet`, after `seek`, read `[u8 dispLen][display]` FIRST and set `e.term = display`, then read `nMeanings`. `dictTerm(i)` returns the **key** (the idx string) — fast, PSRAM. (Callers needing display use `dictGet`.) Keep tier filtering identical.

- [ ] **Step 3: Build**

Run: `~/.platformio/penv/Scripts/pio.exe run`
Expected: `[SUCCESS]`.

- [ ] **Step 4: Commit**
```bash
git add src/Dict.h src/Dict.cpp
git commit -m "Dict: C++ key normaliser + read format v4 base from /dicts/base.*"
```

- [ ] **Step 5: (Controller, on device) flash + verify**

Run (main session): `pio run -t upload && pio run -t uploadfs && python tools/boot_log.py`
Expected boot: `[dict] ready: ...` with the base word count; search works; definitions show display headwords. (Single-source still.)

---

## Phase 2 — Multi-source engine

### Task 2.1: Engine data model + types

**Files:** Modify `src/Dict.h`

- [ ] **Step 1: Add source/mode types and the source API**

Add to `src/Dict.h`:
```cpp
enum DictMode { MODE_ADDITIVE = 0, MODE_OVERRIDE = 1 };

struct DictSourceInfo {
    String   name;
    bool     enabled;
    int      priority;   // 0 = highest
    DictMode mode;
    DictTier floor;
    bool     onSD;
};

// Sources are presented in PRIORITY order (index 0 = highest priority).
int  dictSourceCount();
bool dictGetSource(int orderIdx, DictSourceInfo& out);
void dictSetSourceEnabled(int orderIdx, bool enabled);
void dictSetSourceMode(int orderIdx, DictMode mode);
void dictSetSourceFloor(int orderIdx, DictTier floor);
void dictMoveSource(int orderIdx, int dir);   // dir -1 = raise priority, +1 = lower
```
Keep the existing surface (`dictBegin`, `dictSetTier`, `dictTier`, `dictStatus`, `dictCount`, `dictTerm`, `dictGet`, `dictFind`, `dictLowerBound`, `posName`) — they now operate over the merged view.

- [ ] **Step 2: Build (header only; no impl yet — expect link/use errors only where referenced)**

Run: `~/.platformio/penv/Scripts/pio.exe run`
Expected: `[SUCCESS]` (nothing references the new declarations yet).

- [ ] **Step 3: Commit**
```bash
git add src/Dict.h
git commit -m "Dict.h: multi-source types and source-settings API"
```

### Task 2.2: Source discovery + per-source index load

**Files:** Modify `src/Dict.cpp`

- [ ] **Step 1: Implement discovery and load**

Replace the single-source state with a `Source` array. Each `Source` holds: `name`, `fs*` (LittleFS/SD_MMC), `idxBuf` (PSRAM, whole `.idx`), parallel arrays `key[]`(const char*), `dataOff[]`(u32), `wmin[]`(u8), `count`, an open `.dat` File, and settings (`enabled, priority, mode, floor`). Implement:
```cpp
// pseudocode contract — full impl:
// scanDir(fs, "/dicts"): for each "*.meta", read name/mode/floor; require name.idx & name.dat;
//   if a source with that name exists (flash) and this is SD, SD shadows it (replace fs+files);
//   else register new. Load <name>.idx into PSRAM, parse to key/dataOff/wmin arrays (v4).
// dictBegin(tier): LittleFS.begin(); scan LittleFS /dicts; SD_MMC mount; scan SD /dicts;
//   load NVS settings per source name (key "src:<name>" -> packed enabled|mode|floor|priority),
//   defaulting from .meta; sort sources by priority; rebuildView(); status = "<n> dicts, <m> words".
```
Provide the full C++ (mirror the prior v3 loader's PSRAM parsing per source; `floor`/`mode` parsed from meta strings via small helpers `tierFromName`/`modeFromName`). On parse/version failure for a source, skip that source with a logged status (don't abort the engine). If NO source loads, fall back to embedded `WORDS`.

- [ ] **Step 2: Build**
Run: `~/.platformio/penv/Scripts/pio.exe run` → `[SUCCESS]` (rebuildView/dictGet still stubbed to base only is fine if compiled; but implement rebuildView in 2.3 before flashing).

- [ ] **Step 3: Commit**
```bash
git add src/Dict.cpp
git commit -m "Dict: discover and load multiple sources from /dicts (flash + SD)"
```

### Task 2.3: Merged tier-filtered view + lookup/merge

**Files:** Modify `src/Dict.cpp`

- [ ] **Step 1: Implement the merged view and merge lookup**

```cpp
// MergedEntry { const char* key; uint8_t contribCount; uint16_t contrib[K]; }
//   contrib = source order indices that hold this key AND pass the active tier
//   (effective min tier = max(source.floor, wmin) <= activeTier), in priority order.
// rebuildView():
//   k-way merge of all enabled sources' sorted key arrays (skip entries failing tier);
//   dedupe by key; for each distinct key collect contributing source-order indices in
//   priority order; store in PSRAM arrays s_viewKey[]/s_viewContrib... ; s_count = distinct.
// dictCount() = s_count; dictTerm(i) = s_viewKey[i]; dictLowerBound/dictFind binary-search s_viewKey.
// dictGet(i, e):
//   e.meanings.clear(); display = "";
//   for each src-order idx in entry.contrib (priority order):
//       read that source's .dat at its dataOff for this key;
//       if display empty: display = that entry's display;
//       for each meaning: if max(src.floor, meaning.minTier) <= activeTier <= meaning.maxTier: push;
//       if src.mode == MODE_OVERRIDE: break;   // stop lower sources for this key
//   e.term = display; apply exclusions (Phase 6 hook, no-op for now); return !e.meanings.empty();
```
Provide full C++. Each source caches its `.dat` File handle. Reading a key from a source needs its dataOffset — store per-merged-entry the dataOffset per contributing source (so no re-search): widen the contrib record to `{u16 srcIdx; u32 dataOff;}`. `dictSetTier` calls `rebuildView()`.

- [ ] **Step 2: Build**
Run: `~/.platformio/penv/Scripts/pio.exe run` → `[SUCCESS]`.

- [ ] **Step 3: Commit**
```bash
git add src/Dict.cpp
git commit -m "Dict: merged tier-filtered view + per-word mode-driven merge"
```

### Task 2.4: Source settings (NVS) + reorder/enable/mode/floor

**Files:** Modify `src/Dict.cpp`

- [ ] **Step 1: Implement the source-settings API + persistence**

Implement `dictSourceCount/dictGetSource` (priority order), and the mutators `dictSetSourceEnabled/Mode/Floor` and `dictMoveSource` (swap priority with neighbour, renumber, re-sort, persist, `rebuildView()`). Persist per source to NVS namespace `dict`, key `s_<name>` packing `enabled(1) | mode(1) | floor(2) | priority(8)` as a `uint32`. Load in `dictBegin`. Provide full C++.

- [ ] **Step 2: Build**
Run: `~/.platformio/penv/Scripts/pio.exe run` → `[SUCCESS]`.

- [ ] **Step 3: Commit**
```bash
git add src/Dict.cpp
git commit -m "Dict: source settings (enable/priority/mode/floor) persisted to NVS"
```

- [ ] **Step 4: (Controller, on device) flash + verify single-source still correct**

Run (main session): `pio run -t upload && pio run -t uploadfs && python tools/boot_log.py`
Expected: boots with base only (one source), search/browse/definitions identical to before; status shows "1 dict". (Multi-source proven once mythology lands in Phase 5.)

---

## Phase 3 — Keyboard / charset

### Task 3.1: Add space, hyphen, apostrophe keys

**Files:** Modify `src/main.cpp`

- [ ] **Step 1: Extend the keyboard model**

Re-introduce a 4th row. Add key types and build it. Replace `buildKeyboard`'s tail to add row 4 with space (wide), `-`, `'`. Keep `KB_TOP`/`KEY_H`/`ROW_PITCH` but recompute so 4 rows fit above the tab bar (e.g. `KB_TOP=104, KEY_H=22, ROW_PITCH=27` → rows 104/131/158/185, last ends 207, gap 3). Provide the exact `buildKeyboard` row-4 block:
```cpp
    // Row 4: space + hyphen + apostrophe
    {
        int y = KB_TOP + ROW_PITCH * 3;
        int spw = 150, sw = 44, gap = 6;
        int total = spw + gap + sw + gap + sw;
        int x = (SCREEN_W - total) / 2;
        g_keys.push_back({x, y, spw, KEY_H, ' ', K_SPACE});
        g_keys.push_back({x + spw + gap, y, sw, KEY_H, '-', K_CHAR});
        g_keys.push_back({x + spw + gap + sw + gap, y, sw, KEY_H, '\'', K_CHAR});
    }
```
Re-add `K_SPACE` to the `KeyType` enum and `drawKey`/`handleSearch` switch (space appends `' '`). For `-`/`'` (type `K_CHAR`): they append the char to `g_query`; the dead-key dim check (`g_valid[c-'a']`) must be guarded so non-letters are never treated as dead (skip dimming when `c < 'a' || c > 'z'`). The normalised search already drops `'` and trims, so typing them is safe.

- [ ] **Step 2: Adjust dead-key handling in buildResults**

`buildResults` computes `g_valid[26]` for a-z only; ensure `-`/`'`/space keys are always enabled (not dimmed). In `drawKey`, change `bool dim = (k.type==K_CHAR) && k.c>='a' && k.c<='z' && !g_valid[k.c-'a'];`.

- [ ] **Step 3: Build**
Run: `~/.platformio/penv/Scripts/pio.exe run` → `[SUCCESS]`.

- [ ] **Step 4: Commit**
```bash
git add src/main.cpp
git commit -m "Keyboard: add space, hyphen and apostrophe keys (4-row layout)"
```

- [ ] **Step 5: (Controller, on device) flash + verify layout**
Run: `pio run -t upload`. On device: keyboard shows 4 rows; `-`/`'`/space tap and appear; letters still dim correctly; nothing overlaps the tab bar.

---

## Phase 4 — Settings UI (More → Dictionaries)

### Task 4.1: Dictionaries screen

**Files:** Modify `src/main.cpp`

- [ ] **Step 1: Add a Dictionaries screen reachable from More**

Add `SCR_DICTS` to the `Screen` enum. Add a More-screen button "Dictionaries" (replace one grid slot or add a 5th button) that sets `g_screen = SCR_DICTS` and draws it. Implement `drawDicts()`: a scrollable list (reuse the drag-scroll pattern or simple paged list) where each row shows: source name; an `[on]`/`[off]` enable pill; `▲`/`▼` chevron buttons; a `Add`/`Ovr` mode pill; a tier-floor pill cycling Safe→Mild→Teen→Full. A back button returns to More. Provide full `drawDicts()` + `handleDicts(Tap)` calling the engine mutators (`dictSetSourceEnabled/Mode/Floor`, `dictMoveSource`) then redrawing. Tap targets ≥ ~28px.

- [ ] **Step 2: Route the screen**

Add `SCR_DICTS` to `loop()` dispatch and `redrawCurrent()`.

- [ ] **Step 3: Build**
Run: `~/.platformio/penv/Scripts/pio.exe run` → `[SUCCESS]`.

- [ ] **Step 4: Commit**
```bash
git add src/main.cpp
git commit -m "UI: More -> Dictionaries screen (enable/reorder/mode/floor)"
```

- [ ] **Step 5: (Controller, on device) flash + verify (base only)**
Run: `pio run -t upload`. On device: More→Dictionaries lists "Base (WordNet)"; toggling/floor persist across reboot. (Reorder meaningful once a 2nd dict exists.)

---

## Phase 5 — Greek-mythology content

### Task 5.1: Curated source JSON

**Files:** Create `tools/sources/mythology.json`

- [ ] **Step 1: Author ~150 entries**

Create `tools/sources/mythology.json` — an array of entries:
```json
[
  {"term":"Zeus","pos":"name","tier":"safe",
   "defs":["King of the Greek gods, ruler of Mount Olympus and god of the sky and thunder.",
           "Youngest son of the Titans Cronus and Rhea; brother and husband of Hera."],
   "example":"Zeus hurled thunderbolts at those who defied him."},
  {"term":"Medusa","pos":"name","tier":"safe",
   "defs":["A Gorgon with snakes for hair whose gaze turned onlookers to stone; slain by Perseus."]},
  {"term":"Mount Olympus","pos":"name","tier":"safe",
   "defs":["The highest mountain in Greece, home of the twelve Olympian gods."]}
]
```
Write **original, concise** wording (do not copy licensed encyclopedia text). Cover: Olympians (Zeus, Hera, Poseidon, Demeter, Athena, Apollo, Artemis, Ares, Aphrodite, Hephaestus, Hermes, Hestia, Dionysus), key Titans, heroes (Heracles, Perseus, Theseus, Achilles, Odysseus, Jason), monsters (Medusa, Minotaur, Hydra, Cerberus, Chimera, Cyclops, Sphinx), places (Mount Olympus, Underworld, Troy), concepts (Ambrosia, Oracle). Mark any mature myth with a higher `tier`.

- [ ] **Step 2: Commit**
```bash
git add tools/sources/mythology.json
git commit -m "Add curated Greek-mythology source (~150 entries)"
```

### Task 5.2: Supplementary builder

**Files:** Create `tools/build_supp.py`

- [ ] **Step 1: Write the builder**

Create `tools/build_supp.py` producing v4 `<name>.{idx,dat,meta}` from a source JSON:
```python
#!/usr/bin/env python3
"""Build a supplementary dictionary (format v4) from a JSON source.
Usage: python tools/build_supp.py tools/sources/mythology.json mythology --mode additive --floor safe"""
import json, os, struct, sys
from normalize import norm_key
TIER = {"safe":0,"mild":1,"teen":2,"full":3}; POS = {"noun":0,"verb":1,"adjective":2,"adverb":3,"name":4,"other":4}
def main():
    src, name = sys.argv[1], sys.argv[2]
    mode = "additive"; floor = "safe"
    if "--mode" in sys.argv: mode = sys.argv[sys.argv.index("--mode")+1]
    if "--floor" in sys.argv: floor = sys.argv[sys.argv.index("--floor")+1]
    entries = json.load(open(src, encoding="utf-8"))
    rows = []
    for e in entries:
        disp = e["term"].strip(); key = norm_key(disp)
        if not key: continue
        tier = TIER.get(e.get("tier","safe"),0); pc = POS.get(e.get("pos","name"),4)
        ms = [(tier, pc, d.strip(), (e.get("example","") if i==0 else "").strip())
              for i,d in enumerate(e["defs"]) if d.strip()]
        if ms: rows.append((key, disp, ms))
    rows.sort(key=lambda r: r[0])
    out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data", "dicts")
    os.makedirs(out, exist_ok=True)
    offsets = {}
    with open(os.path.join(out, name+".dat"), "wb") as dat:
        for key, disp, ms in rows:
            offsets[key] = dat.tell()
            db = disp.encode("utf-8")[:255]; dat.write(struct.pack("<B", len(db)) + db)
            dat.write(struct.pack("<B", len(ms)))
            for tier, pc, d, ex in ms:
                tr = tier | (3 << 2)  # minTier=tier, maxTier=FULL
                de = d.encode("utf-8")[:600]; ee = ex.encode("utf-8")[:400]
                dat.write(struct.pack("<B", tr) + struct.pack("<B", pc))
                dat.write(struct.pack("<H", len(de)) + de); dat.write(struct.pack("<H", len(ee)) + ee)
    with open(os.path.join(out, name+".idx"), "wb") as idx:
        idx.write(b"DIDX"); idx.write(struct.pack("<II", 4, len(rows)))
        for key, disp, ms in rows:
            wmin = min(m[0] for m in ms)
            idx.write(struct.pack("<I", offsets[key])); idx.write(struct.pack("<B", wmin))
            idx.write(key.encode("ascii") + b"\x00")
    with open(os.path.join(out, name+".meta"), "w", encoding="utf-8") as m:
        m.write(f"name={name.title()}\nversion=supp-2026.06\nmode={mode}\nfloor={floor}\nformat=4\n")
    print(f"{name}: {len(rows)} entries -> data/dicts/{name}.idx/.dat/.meta")
if __name__ == "__main__": raise SystemExit(main())
```

- [ ] **Step 2: Build the mythology dict + verify**

Run: `python tools/build_supp.py tools/sources/mythology.json mythology --mode additive --floor safe`
Then extend `verify_dict.py` to also verify `data/dicts/mythology.*` if present (same asserts; check `zeus`/`medusa` resolve with display `Zeus`/`Medusa`). Run `python tools/verify_dict.py`.
Expected: mythology entries verify.

- [ ] **Step 3: Commit**
```bash
git add tools/build_supp.py tools/verify_dict.py
git commit -m "Add supplementary-dict builder; build Greek mythology test dict"
```

- [ ] **Step 4: (Controller, on device) flash data + verify multi-source**

Run (main session): `pio run -t uploadfs && python tools/boot_log.py`
Expected: status shows 2 dicts. On device: search `zeus` → Zeus entry (from mythology); More→Dictionaries lists Base + Mythology, reorder/enable works; with Mythology=additive a word in both shows merged senses; set Mythology=override and a shared word shows only its senses.
**Note to user:** the mythology files are bundled into flash via `uploadfs`. To test the **SD path** instead, copy `data/dicts/mythology.{idx,dat,meta}` to `/dicts/` on a FAT32 SD card — I'll prompt you when we test that.

---

## Phase 6 — Exclusions (runtime drop-in)

### Task 6.1: Exclusion file format + loader

**Files:** Modify `src/Dict.cpp`, create `data/dicts/exclude/example.excl`

- [ ] **Step 1: Define + sample file**

Create `data/dicts/exclude/example.excl` (committed under `tools/` sample? data is git-ignored — instead create `tools/sample-exclusions/extra-rude.excl` and document copying it). Format:
```
# action: gate:full        (or: hide)
# scope: all               (or: base,mythology)
shitshow
arsehattery
```
First two `#`-prefixed lines are directives (`action:` and `scope:`), rest are keys (normalised on load).

- [ ] **Step 2: Load + apply**

In `Dict.cpp`: scan `/dicts/exclude/*.excl` (LittleFS + SD) into a small list of `Exclusion{enabled, action(HIDE|GATE), gateTier, scopeAll, scopeNames[], keys(set)}`. NVS toggle `x_<filename>`. In `rebuildView()` and `dictGet`, for a key in an enabled in-scope exclusion: HIDE → drop from view / yield no meanings; GATE → raise its effective min tier to `gateTier` (affects view inclusion + meaning visibility). Provide full C++. Add `int dictExclusionCount(); bool dictGetExclusion(int,String&,bool&); void dictSetExclusionEnabled(int,bool);` to `Dict.h`.

- [ ] **Step 3: Build**
Run: `~/.platformio/penv/Scripts/pio.exe run` → `[SUCCESS]`.

- [ ] **Step 4: Commit**
```bash
git add src/Dict.h src/Dict.cpp tools/sample-exclusions/extra-rude.excl
git commit -m "Dict: runtime drop-in exclusion files (.excl) with action/scope"
```

### Task 6.2: Exclusions in the settings UI

**Files:** Modify `src/main.cpp`

- [ ] **Step 1: Add an Exclusions subsection to the Dictionaries screen**

Below the source list in `drawDicts`, list each `.excl` file with an enable toggle (calls `dictSetExclusionEnabled`). Provide the code.

- [ ] **Step 2: Build + commit**
Run: `~/.platformio/penv/Scripts/pio.exe run` → `[SUCCESS]`.
```bash
git add src/main.cpp
git commit -m "UI: exclusion-file toggles in the Dictionaries screen"
```

- [ ] **Step 3: (Controller, on device) flash + verify**
Copy a sample `.excl` into `data/dicts/exclude/`, `pio run -t uploadfs`, test: enabling a `gate:full` exclusion hides its words below Full; `hide` removes them entirely; scope limits which dicts are affected.

---

## Phase 7 — Documentation

### Task 7.1: Docs

**Files:** Create `docs/dictionaries.md`; modify `README.md`

- [ ] **Step 1: Write `docs/dictionaries.md`**

Cover: the `/dicts/` layout (flash + SD, SD shadows same-named flash dict); format v4 (`.idx`/`.dat`/`.meta`) with the byte layouts; the `.excl` format (action/scope); how to author a supplementary dict (JSON schema + `build_supp.py` usage) and install it (uploadfs or SD copy); the core classification lists (`words_offensive/adult/mild.txt`, `core_harmful.txt`, `sense_labels.tsv`, `overrides.tsv`) and how they feed the base build; the More→Dictionaries UI (enable/reorder/mode/floor, exclusions); and the keyboard (`-`, space, `'`).

- [ ] **Step 2: Update README**

Add a "Supplementary dictionaries" section summarising the feature and linking to `docs/dictionaries.md`; update the build commands (`build_dict.py` now writes `data/dicts/base.*`; `build_supp.py` for add-ons).

- [ ] **Step 3: Commit**
```bash
git add docs/dictionaries.md README.md
git commit -m "Docs: supplementary dictionaries, format v4, classification lists"
```

---

## Final integration & merge

### Task 8.1: Full acceptance + merge

- [ ] **Step 1: (Controller) full build + flash**
`pio run -t upload && pio run -t uploadfs && python tools/boot_log.py` → boots, 2 dicts, base word count + mythology.

- [ ] **Step 2: On-device acceptance checklist**
- Base search/browse/definition unchanged; tiers + PIN still gate (fuck/shitless Full-only; cock rooster at Safe; fart clean at Mild).
- `zeus`/`mount olympus` (type with space) resolve to Mythology entries with proper display case.
- More→Dictionaries: enable/disable Mythology; reorder above/below Base; Additive merges senses, Override replaces; per-dict floor hides a dict below its tier; all persist across reboot.
- Exclusions: a `.excl` hides/gates its words per action+scope; toggle persists.
- Keyboard: `-`, space, `'` work; letters dim correctly.

- [ ] **Step 3: Merge + tag**
```bash
git checkout main
git merge --no-ff feature/supplementary-dictionaries -m "Merge: supplementary dictionaries (multi-source engine, format v4)"
git push origin main
git tag -a v1.3 -m "v1.3 - supplementary dictionaries: multi-source engine, tiers per dict, exclusions, Greek mythology"
git push origin v1.3
```

---

## Self-review notes (author)

- **Spec coverage:** multi-source folder load flash+SD (2.2), toggle/reorder/mode/floor (2.4, 4.1), per-word mode merge (2.3), display+key format v4 (1.x), per-dict floor + per-entry tiers (2.3), runtime exclusions (6.x), keyboard -/space/' (3.1), mythology test (5.x), core lists documented (7.1), SD-shadow (2.2). All covered.
- **Type consistency:** `DictMode{MODE_ADDITIVE,MODE_OVERRIDE}`, `DictSourceInfo`, `dictMoveSource(dir -1/+1)`, `dictNormalizeKey`, `norm_key`, v4 layout (`[u8 dispLen][display]` in dat, `[u8 wordMinTier]` in idx), `tierRange=minTier|(maxTier<<2)` used consistently across Python + C++ tasks.
- **Device steps** all marked "(Controller, on device)"; subagents build-only.
- **Note:** Phase 2 is the largest task group; if an implementer subagent stalls on the k-way merge, split Task 2.3 into "view build" and "merge lookup" sub-tasks.
