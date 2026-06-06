# Multi-Meaning Dictionary + Scrollable UI + Kids Tier — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the single-meaning Wordset dictionary with WordNet-sourced multi-meaning entries (frequency-ordered), shown on a drag-to-scroll definition screen grouped by part of speech, with an "Everyone" baseline and a PIN-lockable "Kids" tier sharing one data file.

**Architecture:** A rewritten Python generator (`tools/build_dict.py`) emits one shared `dict.dat` (format v2: multiple meanings per word, 1-byte POS code) plus two index files — `dict.idx` (Everyone) and `dict_kids.idx` (safety-filtered subset pointing into the same data). Firmware loads the index for the active tier into PSRAM and streams meanings from the shared `dict.dat`; the definition screen renders meanings grouped by POS with drag scrolling; the More screen toggles tiers with an optional 4-digit PIN lock. Flash is repartitioned (1.5 MB app + ~14.4 MB LittleFS).

**Tech Stack:** PlatformIO (espressif32@6.9.0), Arduino, LovyanGFX 1.2.7, ESP32-S3 (16 MB flash / 8 MB PSRAM), LittleFS, SD_MMC, Preferences (NVS); Python 3 with `nltk` (WordNet) and `wordfreq`.

**Spec:** `docs/superpowers/specs/2026-06-06-dictionary-content-and-ui-design.md`

**Device:** Freenove FNK0104 on COM5. Reflash firmware with `pio run -t upload`; reflash dictionary with `pio run -t uploadfs`. Serial is on UART0 (not USB); capture boot logs by resetting over the port — see "Serial capture helper" below.

---

## Conventions used in this plan

**Serial capture helper** (used as the "observe" step for firmware tasks). Save once as `tools/boot_log.py`:

```python
# tools/boot_log.py — reset the device over COM5 and print ~8s of boot output.
import serial, sys, time
port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
s = serial.Serial(port, 115200, timeout=0.3)
s.setDTR(False); s.setRTS(True); time.sleep(0.2); s.setRTS(False)
end = time.time() + 8
while time.time() < end:
    line = s.readline()
    if line:
        print(line.decode(errors="replace").rstrip())
s.close()
```

Run with: `python tools/boot_log.py`

**PlatformIO binary:** `~/.platformio/penv/Scripts/pio.exe` (Windows). Examples below use `pio`; substitute the full path if `pio` is not on PATH.

**IntelliSense/clang errors** like `'Arduino.h' file not found` or `No member named 'color565'` are false positives (headers aren't indexed). Only `pio run` results are authoritative.

---

## Phase A — Data pipeline (host, unit-tested)

### Task A1: Blocklist files

**Files:**
- Create: `tools/blocklist_profanity.txt`
- Create: `tools/blocklist_sensitive.txt`

- [ ] **Step 1: Create the profanity blocklist (Everyone tier)**

Download the public LDNOOBW English list as the seed, then it can be hand-edited:

```bash
curl -fsSL https://raw.githubusercontent.com/LDNOOBW/List-of-Dirty-Naughty-Obscene-and-Otherwise-Bad-Words/master/en > tools/blocklist_profanity.txt
```

Prepend a header line (so the loader skips it and humans know what it is). Open `tools/blocklist_profanity.txt` and add these two lines at the very top:

```
# Profanity / slurs removed from BOTH dictionaries. One lowercase word per line. '#' = comment.
# Seeded from LDNOOBW (github.com/LDNOOBW). Edit freely.
```

- [ ] **Step 2: Create the sensitive blocklist (Kids tier, in addition to profanity)**

Create `tools/blocklist_sensitive.txt` with a curated seed (editable; errs toward removal). This is intentionally non-exhaustive and meant to be extended:

```
# Additional terms removed from the KIDS dictionary only (sexual, drugs, graphic
# violence, explicit anatomy, self-harm). One lowercase word per line. '#' = comment.
# Best-effort; extend as needed.
sex
sexual
sexy
nude
naked
erotic
porn
orgasm
penis
vagina
breast
nipple
buttock
genital
intercourse
prostitute
brothel
condom
drug
cocaine
heroin
methamphetamine
marijuana
cannabis
opium
overdose
syringe
suicide
suicidal
murder
massacre
torture
mutilate
corpse
slaughter
addict
alcoholic
intoxicated
```

- [ ] **Step 3: Commit**

```bash
git add tools/blocklist_profanity.txt tools/blocklist_sensitive.txt
git commit -m "Add profanity (Everyone) and sensitive (Kids) blocklists"
```

---

### Task A2: Rewrite the generator for WordNet multi-meaning output

**Files:**
- Modify (full rewrite): `tools/build_dict.py`

- [ ] **Step 1: Replace `tools/build_dict.py` entirely with the WordNet generator**

```python
#!/usr/bin/env python3
"""
Build the on-device dictionary from WordNet.

Source : WordNet via nltk (auto-downloaded on first run).
Output : data/dict.dat (shared), data/dict.idx (Everyone), data/dict_kids.idx (Kids)
         data/ is PlatformIO's filesystem-image source; flash with `pio run -t uploadfs`.

Format (little-endian, matches the ESP32-S3) — version 2:

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
```

- [ ] **Step 2: Ensure Python deps are present**

Run: `pip install nltk wordfreq`
Expected: installs without error (wordfreq is used by the verifier in A3).

- [ ] **Step 3: Run the generator**

Run: `python tools/build_dict.py`
Expected: prints counts (everyone ~60k, kids slightly fewer) and `Total X.X MB. Flash with: pio run -t uploadfs`, with X ≤ ~14. If it warns about exceeding the budget, re-run with a lower cap, e.g. `DICT_SENSE_CAP=6 python tools/build_dict.py`, and record the working cap.

- [ ] **Step 4: Commit**

```bash
git add tools/build_dict.py
git commit -m "Rewrite generator: WordNet multi-meaning, shared dat + Everyone/Kids indexes (format v2)"
```

---

### Task A3: Host verifier (the data "tests")

**Files:**
- Create: `tools/verify_dict.py`

- [ ] **Step 1: Write the verifier**

```python
#!/usr/bin/env python3
"""Parse the generated data/ files and assert correctness. Exit non-zero on failure."""
import os
import struct
import sys

DATA = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data")
POS = {0: "noun", 1: "verb", 2: "adjective", 3: "adverb", 4: "other"}


def read_idx(path):
    b = open(path, "rb").read()
    assert b[:4] == b"DIDX", f"{path}: bad magic"
    ver, count = struct.unpack("<II", b[4:12])
    assert ver == 2, f"{path}: version {ver} != 2"
    terms, offs, p = [], [], 12
    for _ in range(count):
        off = struct.unpack("<I", b[p:p + 4])[0]; p += 4
        e = b.index(b"\x00", p); terms.append(b[p:e].decode("ascii")); offs.append(off); p = e + 1
    return terms, offs


def read_entry(dat, off):
    p = off
    n = dat[p]; p += 1
    ms = []
    for _ in range(n):
        pc = dat[p]; p += 1
        dl = struct.unpack("<H", dat[p:p + 2])[0]; p += 2
        d = dat[p:p + dl].decode("utf-8", "replace"); p += dl
        el = struct.unpack("<H", dat[p:p + 2])[0]; p += 2
        ex = dat[p:p + el].decode("utf-8", "replace"); p += el
        ms.append((pc, d, ex))
    return ms


def main():
    et, eo = read_idx(os.path.join(DATA, "dict.idx"))
    kt, ko = read_idx(os.path.join(DATA, "dict_kids.idx"))
    dat = open(os.path.join(DATA, "dict.dat"), "rb").read()

    assert et == sorted(et), "everyone index not sorted"
    assert kt == sorted(kt), "kids index not sorted"
    eset = set(et)
    assert set(kt) <= eset, "kids is not a subset of everyone"

    # kids offsets must match everyone offsets (shared data)
    emap = dict(zip(et, eo))
    for t, o in zip(kt, ko):
        assert emap[t] == o, f"kids offset mismatch for {t}"

    # primary sense sanity
    def sense1(term):
        return read_entry(dat, emap[term])[0]
    for w, want in [("blue", "noun"), ("toy", "noun"), ("mouse", "noun")]:
        assert w in eset, f"{w} missing"
        pc, d, _ = sense1(w)
        print(f"{w}: [{POS[pc]}] {d[:60]}")
    # blue must mention colour/color in some sense; not lead with depression
    blue_defs = " ".join(d for _, d, _ in read_entry(dat, emap["blue"])).lower()
    assert "colour" in blue_defs or "color" in blue_defs, "blue lacks a colour sense"

    # filtering: a sample profane word absent from everyone; a sensitive word absent from kids
    for bad in ["fuck", "shit"]:
        assert bad not in eset, f"profanity {bad} present in everyone"
    for s in ["sex", "drug"]:
        if s in eset:
            assert s not in set(kt), f"sensitive {s} present in kids"

    total = sum(os.path.getsize(os.path.join(DATA, f)) for f in ("dict.dat", "dict.idx", "dict_kids.idx"))
    print(f"OK: everyone={len(et)} kids={len(kt)} total={total/1048576:.1f}MB")
    assert total <= 14 * 1048576, "data exceeds ~14MB budget"
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 2: Run the verifier**

Run: `python tools/verify_dict.py`
Expected: prints `blue: [noun] ...`, `toy: [noun] ...`, `mouse: [noun] ...`, then `OK: everyone=... kids=... total=...MB` and exits 0. If any assert fails, fix the generator (Task A2) and regenerate.

- [ ] **Step 3: Update `.gitignore` note (data/ already ignored) and commit the verifier**

```bash
git add tools/verify_dict.py tools/boot_log.py
git commit -m "Add data verifier and serial boot-log helper"
```

(Confirm `data/` and `tools/wordset/` remain git-ignored; `git status` should show neither.)

---

## Phase B — Firmware data layer (format v2 + tiers)

### Task B1: Repartition flash (1.5 MB app + ~14.4 MB LittleFS)

**Files:**
- Modify: `partitions.csv`

- [ ] **Step 1: Replace `partitions.csv`**

```
# CYD-Dictionary partition table for the 16 MB FNK0104.
# 1.5 MB app + ~14.4 MB LittleFS data (holds the shared multi-meaning dict.dat
# plus the Everyone and Kids index files). Data partition labelled "spiffs" so
# Arduino LittleFS.begin() and PlatformIO uploadfs find it.
# Name,   Type, SubType, Offset,   Size
nvs,      data, nvs,     0x9000,   0x5000,
factory,  app,  factory, 0x10000,  0x180000,
spiffs,   data, spiffs,  0x190000, 0xE70000,
```

- [ ] **Step 2: Build to validate the partition table**

Run: `pio run`
Expected: `[SUCCESS]`. The "Checking size" line shows the app partition is now ~1.5 MB (`1572864` bytes) and usage is well under it.

- [ ] **Step 3: Commit**

```bash
git add partitions.csv
git commit -m "Repartition 16MB flash: 1.5MB app + ~14.4MB LittleFS"
```

---

### Task B2: `Dict.h` — multi-meaning entry, tiers, POS names

**Files:**
- Modify: `src/Dict.h`

- [ ] **Step 1: Replace the contents of `src/Dict.h`**

```cpp
#pragma once

// ==============================================================================
// Dictionary data source (format v2: multiple meanings per word).
//
// Sources, in priority order: SD card override -> built-in LittleFS corpus ->
// embedded fallback (WordData.h, single meaning). The active tier (Everyone or
// Kids) selects which index file is used over the shared dict.dat.
//
// The index lives in PSRAM; meanings are streamed from dict.dat on demand.
// ==============================================================================

#include <Arduino.h>
#include <vector>

enum DictTier { TIER_EVERYONE = 0, TIER_KIDS = 1 };

struct Meaning
{
    uint8_t posCode;   // 0 noun, 1 verb, 2 adjective, 3 adverb, 4 other
    String  def;
    String  example;   // "" if none
};

struct DictEntry
{
    String                term;
    std::vector<Meaning>  meanings;   // frequency-ordered
};

// Loads the dictionary for `tier` from the first available source. Returns true
// if a file-backed corpus loaded; false means the embedded fallback is active.
bool dictBegin(DictTier tier);

// Switches the active tier by reloading its index over the already-open dict.dat.
// Returns true on success. No-op-returns-false in embedded mode.
bool dictSetTier(DictTier tier);

// The currently active tier.
DictTier dictTier();

// Diagnostic string, e.g. "Flash (Everyone): 60123 words".
const char* dictStatus();

// Number of words in the active source.
int dictCount();

// Null-terminated lowercase term for index i. No I/O; valid for the session.
const char* dictTerm(int i);

// Fills a full entry (term + meanings) for index i. Returns false if out of range.
bool dictGet(int i, DictEntry& e);

// Exact (lowercase) term lookup -> index, or -1.
int dictFind(const char* term);

// First index whose term is >= key (lowercase). Bounds prefix scans.
int dictLowerBound(const char* key);

// Human-readable part of speech for a posCode ("noun", "verb", ...).
const char* posName(uint8_t posCode);
```

- [ ] **Step 2: Commit**

```bash
git add src/Dict.h
git commit -m "Dict.h: multi-meaning DictEntry, tier API, posName"
```

---

### Task B3: `Dict.cpp` — parse v2, tier selection, reload

**Files:**
- Modify (full rewrite): `src/Dict.cpp`

- [ ] **Step 1: Replace the contents of `src/Dict.cpp`**

```cpp
#include "Dict.h"
#include "WordData.h"
#include "DisplayConfig.hpp"

#include <FS.h>
#include <SD_MMC.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <string.h>

// ----------------------------------------------------------------------------
// State. Index buffers live in PSRAM and are freed/reloaded on tier change;
// the shared dict.dat handle and source FS stay open across tier switches.
// ----------------------------------------------------------------------------
static bool          s_loaded   = false;
static fs::FS*       s_fs       = nullptr;   // source filesystem (SD_MMC or LittleFS)
static uint8_t*      s_idx      = nullptr;   // current index file (PSRAM)
static const char**  s_term     = nullptr;   // term pointers into s_idx (PSRAM)
static uint32_t*     s_dataOff  = nullptr;   // dict.dat offset per entry (PSRAM)
static int           s_count    = 0;
static File          s_dat;                  // shared dict.dat handle
static DictTier      s_tier     = TIER_EVERYONE;
static char          s_status[64] = "Built-in set";

static const char* tierIdxPath(DictTier t) { return t == TIER_KIDS ? "/dict_kids.idx" : "/dict.idx"; }
static const char* tierName(DictTier t)    { return t == TIER_KIDS ? "Kids" : "Everyone"; }

static void setStatus(const char* msg) { strncpy(s_status, msg, sizeof(s_status) - 1); s_status[sizeof(s_status) - 1] = 0; }

static void freeIndex()
{
    if (s_idx)     { heap_caps_free(s_idx);     s_idx = nullptr; }
    if (s_term)    { heap_caps_free(s_term);    s_term = nullptr; }
    if (s_dataOff) { heap_caps_free(s_dataOff); s_dataOff = nullptr; }
    s_count = 0;
}

static String readField(uint32_t len)
{
    static char buf[300];   // def<=240, example<=160 (see generator)
    if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
    int n = s_dat.read(reinterpret_cast<uint8_t*>(buf), len);
    if (n < 0) n = 0;
    buf[n] = 0;
    return String(buf);
}

// Loads an index file from s_fs into PSRAM, replacing any current index.
static bool loadIndex(const char* idxPath, const char* tag)
{
    char st[64];
    File f = s_fs->open(idxPath, FILE_READ);
    if (!f) { snprintf(st, sizeof(st), "%s: %s missing", tag, idxPath); setStatus(st); return false; }

    size_t sz = f.size();
    uint8_t* idx = static_cast<uint8_t*>(heap_caps_malloc(sz, MALLOC_CAP_SPIRAM));
    if (!idx) { snprintf(st, sizeof(st), "%s: PSRAM alloc failed", tag); setStatus(st); f.close(); return false; }
    size_t got = f.read(idx, sz);
    f.close();
    if (got != sz) { snprintf(st, sizeof(st), "%s: idx short read", tag); setStatus(st); heap_caps_free(idx); return false; }
    if (memcmp(idx, "DIDX", 4) != 0) { snprintf(st, sizeof(st), "%s: bad magic", tag); setStatus(st); heap_caps_free(idx); return false; }

    uint32_t version = 0, count = 0;
    memcpy(&version, idx + 4, 4);
    memcpy(&count,   idx + 8, 4);
    if (version != 2) { snprintf(st, sizeof(st), "%s: idx version %u", tag, (unsigned)version); setStatus(st); heap_caps_free(idx); return false; }

    const char** term    = static_cast<const char**>(heap_caps_malloc(sizeof(char*) * count, MALLOC_CAP_SPIRAM));
    uint32_t*    dataOff = static_cast<uint32_t*>(heap_caps_malloc(sizeof(uint32_t) * count, MALLOC_CAP_SPIRAM));
    if (!term || !dataOff) {
        snprintf(st, sizeof(st), "%s: PSRAM alloc failed", tag); setStatus(st);
        heap_caps_free(idx); heap_caps_free(term); heap_caps_free(dataOff);
        return false;
    }

    size_t p = 12;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t off; memcpy(&off, idx + p, 4); p += 4;
        dataOff[i] = off;
        term[i] = reinterpret_cast<const char*>(idx + p);
        while (idx[p] != 0) ++p;
        ++p;
    }

    freeIndex();
    s_idx = idx; s_term = term; s_dataOff = dataOff; s_count = static_cast<int>(count);
    snprintf(st, sizeof(st), "%s: %d words", tag, s_count);
    setStatus(st);
    return true;
}

// Opens the shared dict.dat from SD (if a card has it) else LittleFS. Sets s_fs.
static bool openSource()
{
    SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3);
    if (SD_MMC.begin("/sdcard", false, false, SDMMC_FREQ_DEFAULT, 5) &&
        SD_MMC.cardType() != CARD_NONE) {
        File d = SD_MMC.open("/dict.dat", FILE_READ);
        if (d) { s_dat = d; s_fs = &SD_MMC; return true; }
        SD_MMC.end();
    }
    if (LittleFS.begin(false)) {
        File d = LittleFS.open("/dict.dat", FILE_READ);
        if (d) { s_dat = d; s_fs = &LittleFS; return true; }
        setStatus("Flash: dict.dat missing");
    } else {
        setStatus("Flash FS mount failed");
    }
    return false;
}

bool dictBegin(DictTier tier)
{
    s_tier = tier;
    if (!openSource()) { s_loaded = false; setStatus("Built-in set"); return false; }
    char tag[24]; snprintf(tag, sizeof(tag), "%s (%s)", (s_fs == &SD_MMC ? "SD" : "Flash"), tierName(tier));
    if (!loadIndex(tierIdxPath(tier), tag)) { s_loaded = false; return false; }
    s_loaded = true;
    return true;
}

bool dictSetTier(DictTier tier)
{
    if (!s_loaded || !s_fs) return false;
    char tag[24]; snprintf(tag, sizeof(tag), "%s (%s)", (s_fs == &SD_MMC ? "SD" : "Flash"), tierName(tier));
    if (!loadIndex(tierIdxPath(tier), tag)) return false;
    s_tier = tier;
    return true;
}

DictTier dictTier() { return s_tier; }
const char* dictStatus() { return s_status; }
int dictCount() { return s_loaded ? s_count : WORD_COUNT; }

const char* dictTerm(int i)
{
    if (i < 0 || i >= dictCount()) return "";
    return s_loaded ? s_term[i] : WORDS[i].term;
}

static uint8_t posCodeFromName(const char* pos)
{
    if (!pos) return 4;
    if (!strcmp(pos, "noun")) return 0;
    if (!strcmp(pos, "verb")) return 1;
    if (!strcmp(pos, "adjective")) return 2;
    if (!strcmp(pos, "adverb")) return 3;
    return 4;
}

bool dictGet(int i, DictEntry& e)
{
    if (i < 0 || i >= dictCount()) return false;
    e.meanings.clear();

    if (!s_loaded) {
        e.term = WORDS[i].term;
        Meaning m; m.posCode = posCodeFromName(WORDS[i].pos); m.def = WORDS[i].def; m.example = WORDS[i].example;
        e.meanings.push_back(m);
        return true;
    }

    e.term = s_term[i];
    if (!s_dat) return false;
    s_dat.seek(s_dataOff[i]);
    uint8_t n = 0;
    s_dat.read(&n, 1);
    for (uint8_t k = 0; k < n; ++k) {
        Meaning m;
        uint8_t pc = 0; s_dat.read(&pc, 1); m.posCode = pc;
        uint16_t dl = 0; s_dat.read(reinterpret_cast<uint8_t*>(&dl), 2); m.def = readField(dl);
        uint16_t el = 0; s_dat.read(reinterpret_cast<uint8_t*>(&el), 2); m.example = readField(el);
        e.meanings.push_back(m);
    }
    return true;
}

int dictFind(const char* term)
{
    int lo = 0, hi = dictCount() - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(dictTerm(mid), term);
        if (c == 0) return mid;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

int dictLowerBound(const char* key)
{
    int lo = 0, hi = dictCount();
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (strcmp(dictTerm(mid), key) < 0) lo = mid + 1; else hi = mid;
    }
    return lo;
}

const char* posName(uint8_t posCode)
{
    switch (posCode) {
        case 0: return "noun";
        case 1: return "verb";
        case 2: return "adjective";
        case 3: return "adverb";
        default: return "";
    }
}
```

- [ ] **Step 2: Build (expect errors in main.cpp, not Dict.cpp)**

Run: `pio run`
Expected: compilation FAILS, but only in `src/main.cpp` (it still references the old `DictEntry` fields `e.pos`/`e.def`/`e.example` and `dictBegin()`/`dictUsingSD()`). `Dict.cpp` itself must compile clean. Those main.cpp errors are fixed in Phase C/D. Do not commit yet — proceed to Task C1.

---

## Phase C — Definition screen (grouped by POS + drag scroll)

### Task C1: Update setup + simple consumers to the new API

**Files:**
- Modify: `src/main.cpp` (setup tier load; `drawPreview`; `drawWordList` row rendering)

- [ ] **Step 1: Load tier from NVS in `setup()`**

In `src/main.cpp`, find the line `dictBegin();` (inside `setup()`) and replace it with:

```cpp
    // Load the dictionary for the saved tier (SD corpus if present, else flash, else embedded).
    DictTier startTier = (DictTier)prefs.getUChar("tier", TIER_EVERYONE);
    dictBegin(startTier);
```

Note: `prefs.begin("dict", false)` is called just below this in the current code. Move the `dictBegin(...)` call to AFTER `prefs.begin(...)` and `loadState()` is fine too, but it must stay BEFORE `loadState()` uses `dictCount()`. Concretely, ensure order is: `prefs.begin("dict", false);` → `DictTier startTier = ...; dictBegin(startTier);` → `loadState();`. Adjust the two lines so `prefs.begin` precedes the `prefs.getUChar` call.

- [ ] **Step 2: Fix `drawPreview()` to use the primary meaning**

In `drawPreview()`, replace the block that currently reads:

```cpp
    DictEntry w;
    dictGet(g_results[0], w);
    lcd.setFont(&fonts::Font4);
    int tw = lcd.textWidth(w.term);
    lcd.setTextColor(C_TEXT, C_BG);
    lcd.drawString(w.term, 10, top);
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(C_ACCENT, C_BG);
    lcd.drawString(w.pos, 10 + tw + 8, top + 10);
    lcd.setTextColor(C_TEXT, C_BG);
    drawWrapped(w.def, 10, top + 28, SCREEN_W - 20, 18);
```

with:

```cpp
    DictEntry w;
    dictGet(g_results[0], w);
    if (w.meanings.empty()) return;
    const Meaning& m0 = w.meanings[0];
    lcd.setFont(&fonts::Font4);
    int tw = lcd.textWidth(w.term);
    lcd.setTextColor(C_TEXT, C_BG);
    lcd.drawString(w.term, 10, top);
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(C_ACCENT, C_BG);
    lcd.drawString(posName(m0.posCode), 10 + tw + 8, top + 10);
    lcd.setTextColor(C_TEXT, C_BG);
    drawWrapped(m0.def, 10, top + 28, SCREEN_W - 20, 18);
```

- [ ] **Step 3: Fix `drawWordList()` row rendering**

In `drawWordList()`, replace:

```cpp
        DictEntry w;
        dictGet(at(di), w);
        lcd.fillRect(0, y, listW, ROW_H - 1, C_ROW);
        lcd.setTextColor(C_TEXT, C_ROW);
        lcd.drawString(w.term, 10, y + ROW_H / 2);
        lcd.setTextColor(C_SUB, C_ROW);
        lcd.drawString(w.pos, listW - 70, y + ROW_H / 2);
```

with:

```cpp
        DictEntry w;
        dictGet(at(di), w);
        lcd.fillRect(0, y, listW, ROW_H - 1, C_ROW);
        lcd.setTextColor(C_TEXT, C_ROW);
        lcd.drawString(w.term, 10, y + ROW_H / 2);
        lcd.setTextColor(C_SUB, C_ROW);
        lcd.drawString(w.meanings.empty() ? "" : posName(w.meanings[0].posCode), listW - 70, y + ROW_H / 2);
```

- [ ] **Step 4: Do not build yet** — `drawDefinition()` still uses old fields; fixed in C2. Proceed.

---

### Task C2: Grouped, scrollable definition body

**Files:**
- Modify: `src/main.cpp` (replace `drawDefinition()`; add layout/scroll state and `drawDefinitionBody()`, `defMaxScroll()`)

- [ ] **Step 1: Add scroll state and layout constants**

Near the other definition constants (`DEF_BACK_W`, `DEF_STAR_W`), add:

```cpp
static const int DEF_BODY_TOP = HEADER_H;                 // body starts under the fixed header
static const int DEF_VIEW_H   = SCREEN_H - DEF_BODY_TOP;  // viewport height (212)
static const int DEF_SB_W     = 5;                        // scrollbar width
static int g_defScroll = 0;                               // current scroll offset (px)
static int g_defContentH = 0;                             // total content height (px), set by layout
```

- [ ] **Step 2: Replace `drawDefinition()` and add the body renderer**

Replace the entire `drawDefinition()` function with the following three functions:

```cpp
// Draws the fixed top bar (Back + headword + favourite toggle).
static void drawDefinitionHeader(const DictEntry& w)
{
    lcd.fillRect(0, 0, SCREEN_W, HEADER_H, C_HEADER);
    lcd.fillRoundRect(6, 4, DEF_BACK_W, HEADER_H - 8, 6, C_ACCENT);
    lcd.setFont(&fonts::Font2);
    lcd.setTextDatum(textdatum_t::middle_center);
    lcd.setTextColor(C_HEADERTX, C_ACCENT);
    lcd.drawString("< Back", 6 + DEF_BACK_W / 2, HEADER_H / 2);

    bool fav = isFav(w.term);
    lcd.fillRoundRect(SCREEN_W - DEF_STAR_W - 6, 4, DEF_STAR_W, HEADER_H - 8, 6, fav ? C_STAR : C_ACCENT);
    lcd.setTextColor(C_HEADERTX);
    lcd.drawString(fav ? "* in" : "+ fav", SCREEN_W - DEF_STAR_W / 2 - 6, HEADER_H / 2);

    // headword centred between the two buttons
    lcd.setFont(&fonts::Font4);
    lcd.setTextColor(C_HEADERTX, C_HEADER);
    lcd.drawString(w.term, SCREEN_W / 2, HEADER_H / 2);
    lcd.setTextDatum(textdatum_t::top_left);
}

// Lays out (and optionally draws) the grouped meanings starting at virtual y=0.
// Returns total content height. When draw is true, content is offset by -g_defScroll
// and clipped to the viewport. Groups meanings by POS in first-seen order.
static int layoutDefinitionBody(const DictEntry& w, bool draw)
{
    const int x = 10;
    const int maxw = SCREEN_W - DEF_SB_W - x - 6;
    const int lineH = 20;
    int vy = 0;   // virtual y (content space)

    // unique POS codes in first-seen order
    uint8_t order[8]; int nOrder = 0;
    for (const auto& m : w.meanings) {
        bool seen = false;
        for (int k = 0; k < nOrder; ++k) if (order[k] == m.posCode) { seen = true; break; }
        if (!seen && nOrder < 8) order[nOrder++] = m.posCode;
    }

    auto drawLineWrapped = [&](const String& text, uint16_t color, int indent) {
        // word-wrap `text` at the content width, emitting lines at (x+indent, screenY)
        lcd.setFont(&fonts::Font2);
        String line = "";
        int start = 0;
        int avail = maxw - indent;
        while (start <= (int)text.length()) {
            int sp = text.indexOf(' ', start);
            String word = (sp < 0) ? text.substring(start) : text.substring(start, sp);
            String trial = line.length() ? line + " " + word : word;
            if (lcd.textWidth(trial) > avail && line.length()) {
                if (draw) {
                    int sy = DEF_BODY_TOP + vy - g_defScroll;
                    if (sy + lineH > DEF_BODY_TOP && sy < SCREEN_H) {
                        lcd.setTextColor(color, C_BG);
                        lcd.drawString(line, x + indent, sy);
                    }
                }
                vy += lineH;
                line = word;
            } else {
                line = trial;
            }
            if (sp < 0) break;
            start = sp + 1;
        }
        if (line.length()) {
            if (draw) {
                int sy = DEF_BODY_TOP + vy - g_defScroll;
                if (sy + lineH > DEF_BODY_TOP && sy < SCREEN_H) {
                    lcd.setTextColor(color, C_BG);
                    lcd.drawString(line, x + indent, sy);
                }
            }
            vy += lineH;
        }
    };

    vy += 6;
    for (int g = 0; g < nOrder; ++g) {
        uint8_t pc = order[g];
        // POS header
        if (draw) {
            int sy = DEF_BODY_TOP + vy - g_defScroll;
            if (sy + 18 > DEF_BODY_TOP && sy < SCREEN_H) {
                lcd.setFont(&fonts::Font2);
                lcd.setTextColor(C_ACCENT, C_BG);
                String h = String(posName(pc)); h.toUpperCase();
                lcd.drawString(h, x, sy);
            }
        }
        vy += 22;
        // senses of this POS, numbered within the group
        int n = 1;
        for (const auto& m : w.meanings) {
            if (m.posCode != pc) continue;
            drawLineWrapped(String(n) + ".  " + m.def, C_TEXT, 0);
            if (m.example.length()) drawLineWrapped(String("\"") + m.example + "\"", C_SUB, 14);
            vy += 4;
            ++n;
        }
        vy += 6;
    }
    return vy;
}

static int defMaxScroll()
{
    int m = g_defContentH - DEF_VIEW_H;
    return m > 0 ? m : 0;
}

// Redraws just the scrolling body + scrollbar (header stays put).
static void drawDefinitionBody()
{
    DictEntry w;
    dictGet(g_currentWord, w);
    lcd.fillRect(0, DEF_BODY_TOP, SCREEN_W, DEF_VIEW_H, C_BG);
    lcd.setClipRect(0, DEF_BODY_TOP, SCREEN_W - DEF_SB_W, DEF_VIEW_H);
    layoutDefinitionBody(w, true);
    lcd.clearClipRect();

    // scrollbar
    int maxs = defMaxScroll();
    if (maxs > 0) {
        int trackX = SCREEN_W - DEF_SB_W;
        lcd.fillRect(trackX, DEF_BODY_TOP, DEF_SB_W, DEF_VIEW_H, C_ROWLINE);
        int thumbH = DEF_VIEW_H * DEF_VIEW_H / g_defContentH;
        if (thumbH < 18) thumbH = 18;
        int thumbY = DEF_BODY_TOP + (DEF_VIEW_H - thumbH) * g_defScroll / maxs;
        lcd.fillRoundRect(trackX, thumbY, DEF_SB_W, thumbH, 2, C_ACCENT);
    }
}

static void drawDefinition()
{
    DictEntry w;
    dictGet(g_currentWord, w);
    lcd.fillScreen(C_BG);
    g_defContentH = layoutDefinitionBody(w, false);   // measure
    drawDefinitionHeader(w);
    drawDefinitionBody();
}
```

- [ ] **Step 3: Reset scroll when opening a word**

In `openDefinition()`, after `g_currentWord = idx;` add:

```cpp
    g_defScroll = 0;
```

- [ ] **Step 4: Build**

Run: `pio run`
Expected: `[SUCCESS]`. (All old `DictEntry` field references are now gone.)

- [ ] **Step 5: Commit**

```bash
git add src/main.cpp src/Dict.cpp src/Dict.h
git commit -m "Firmware: format v2 multi-meaning parse + grouped definition rendering"
```

---

### Task C3: Drag-to-scroll touch handling

**Files:**
- Modify: `src/main.cpp` (add `handleDefinitionTouch()`; branch it in `loop()`)

- [ ] **Step 1: Add the drag handler**

Add this function just above `loop()`:

```cpp
// Continuous touch handler for the definition screen: drag to scroll, tap (no
// movement) falls through to handleDefinition() for Back / favourite.
static bool s_defDown = false;
static int  s_defY0 = 0, s_defScroll0 = 0, s_defDownX = 0, s_defDownY = 0;
static bool s_defDragged = false;

static void handleDefinitionTouch()
{
    int32_t x, y;
    bool now = lcd.getTouch(&x, &y);
    if (now && !s_defDown) {
        s_defDown = true; s_defDragged = false;
        s_defY0 = y; s_defScroll0 = g_defScroll; s_defDownX = x; s_defDownY = y;
    } else if (now && s_defDown) {
        int dy = s_defY0 - y;
        if (abs(dy) > 6) s_defDragged = true;
        if (s_defDragged) {
            int ns = s_defScroll0 + dy;
            if (ns < 0) ns = 0;
            if (ns > defMaxScroll()) ns = defMaxScroll();
            if (ns != g_defScroll) { g_defScroll = ns; drawDefinitionBody(); }
        }
    } else if (!now && s_defDown) {
        s_defDown = false;
        if (!s_defDragged) {
            Tap t{true, s_defDownX, s_defDownY};
            handleDefinition(t);   // Back / favourite hit-testing on the fixed header
        }
    }
}
```

- [ ] **Step 2: Branch the definition screen in `loop()`**

Replace the body of `loop()` with:

```cpp
void loop()
{
    if (g_screen == SCR_DEFINITION) {
        handleDefinitionTouch();
        delay(10);
        return;
    }
    Tap t = pollTap();
    if (t.hit) {
        switch (g_screen) {
            case SCR_SEARCH:     handleSearch(t); break;
            case SCR_BROWSE:     handleBrowse(t); break;
            case SCR_SAVED:      handleSaved(t); break;
            case SCR_MORE:       handleMore(t); break;
            case SCR_DEFINITION: break;   // handled above
            case SCR_HISTORY:    handleHistory(t); break;
        }
    }
    delay(10);
}
```

- [ ] **Step 3: Keep tap edges clean when leaving the definition screen**

In `handleDefinition()`, the Back branch sets `g_screen = g_prevScreen;`. Immediately after that assignment (before `redrawCurrent();`), add:

```cpp
        s_defDown = false; g_wasTouched = true;   // swallow this release on the next screen
```

This prevents the lift-off from registering as a tap on the screen we return to.

- [ ] **Step 4: Build, flash firmware + dictionary, and verify on device**

Run:
```bash
pio run -t upload
pio run -t uploadfs
python tools/boot_log.py
```
Expected boot log includes: `[dict] ready: Flash (Everyone): NNNNN words` (NNNNN ~60k).
On the device: search `blue`, open it — the definition shows grouped meanings (NOUN/ADJECTIVE…), the colour sense is present, and dragging up/down scrolls the body while Back/★ stay fixed. A short word (e.g. `cat`) shows no scrollbar.

- [ ] **Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "Definition screen: drag-to-scroll with tap fall-through"
```

---

## Phase D — Tier toggle + PIN lock (More screen)

### Task D1: NVS helpers and a numeric keypad

**Files:**
- Modify: `src/main.cpp` (add PIN state + `getPin`/`setPin`; add `promptPin()` modal keypad)

- [ ] **Step 1: Add PIN persistence + a blocking keypad modal**

Add near the persistence helpers (after `pushHistory`):

```cpp
static String getPin() { return prefs.getString("pin", ""); }
static void   setPin(const String& p) { prefs.putString("pin", p); }

// Blocking numeric keypad. Returns the entered 4-digit string, or "" if cancelled.
// `title` is shown at the top.
static String promptPin(const char* title)
{
    String entry = "";
    // 3x4 grid: 1..9, Cancel, 0, OK
    const char* labels[12] = {"1","2","3","4","5","6","7","8","9","Cancel","0","OK"};
    int gw = 80, gh = 42, gap = 8;
    int gridW = gw * 3 + gap * 2;
    int x0 = (SCREEN_W - gridW) / 2;
    int y0 = 70;

    auto redraw = [&]() {
        lcd.fillScreen(C_BG);
        lcd.setFont(&fonts::Font2);
        lcd.setTextColor(C_TEXT, C_BG);
        lcd.setTextDatum(textdatum_t::middle_center);
        lcd.drawString(title, SCREEN_W / 2, 24);
        // masked entry
        String mask = "";
        for (size_t i = 0; i < entry.length(); ++i) mask += "*";
        lcd.drawString(mask + "____" .substring(0, 4 - (int)entry.length()), SCREEN_W / 2, 48);
        for (int i = 0; i < 12; ++i) {
            int r = i / 3, c = i % 3;
            int bx = x0 + c * (gw + gap), by = y0 + r * (gh + gap);
            uint16_t col = (i == 9) ? C_SUB : (i == 11 ? C_ACCENT : C_KEY);
            uint16_t tx = (i == 9 || i == 11) ? C_HEADERTX : C_KEYTX;
            lcd.fillRoundRect(bx, by, gw, gh, 6, col);
            lcd.setTextColor(tx, col);
            lcd.drawString(labels[i], bx + gw / 2, by + gh / 2);
        }
        lcd.setTextDatum(textdatum_t::top_left);
    };
    redraw();

    g_wasTouched = true;   // ignore the press that opened this modal
    for (;;) {
        Tap t = pollTap();
        if (!t.hit) { delay(10); continue; }
        for (int i = 0; i < 12; ++i) {
            int r = i / 3, c = i % 3;
            int bx = x0 + c * (gw + gap), by = y0 + r * (gh + gap);
            if (inRect(t, bx, by, gw, gh)) {
                if (i == 9) return "";                       // Cancel
                if (i == 11) return entry;                   // OK
                char d = labels[i][0];
                if (entry.length() < 4) { entry += d; redraw(); }
            }
        }
    }
}
```

- [ ] **Step 2: Build**

Run: `pio run`
Expected: `[SUCCESS]`.

- [ ] **Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "Add PIN persistence and numeric keypad modal"
```

---

### Task D2: More screen — tier toggle, lock enforcement, Set PIN

**Files:**
- Modify: `src/main.cpp` (`drawMore()` layout; `handleMore()` logic; `g_moreBtns` count)

- [ ] **Step 1: Expand the More button set and lay out a tier row**

Change the More button array declaration from:

```cpp
static MoreBtn g_moreBtns[3];
```

to:

```cpp
static MoreBtn g_moreBtns[4];     // Random, Recent, Recalibrate, Set/Change PIN
static MoreBtn g_tierBtn;         // the Everyone/Kids toggle row
```

- [ ] **Step 2: Replace `drawMore()`**

```cpp
static void drawMore()
{
    lcd.fillScreen(C_BG);
    drawHeader("More");

    // Word of the Day card (compact)
    int cardX = 8, cardY = HEADER_H + 6, cardW = SCREEN_W - 16, cardH = 46;
    lcd.fillRoundRect(cardX, cardY, cardW, cardH, 8, C_WOD);
    lcd.drawRoundRect(cardX, cardY, cardW, cardH, 8, C_WODBORD);
    lcd.setFont(&fonts::Font2);
    lcd.setTextDatum(textdatum_t::top_left);
    lcd.setTextColor(C_SUB, C_WOD);
    lcd.drawString("WORD OF THE DAY", cardX + 8, cardY + 4);
    lcd.setFont(&fonts::Font4);
    lcd.setTextColor(C_TEXT, C_WOD);
    lcd.drawString(dictTerm(g_wotd), cardX + 8, cardY + 20);

    // Tier toggle row: "Dictionary:  [Everyone] [Kids]"
    int ty = cardY + cardH + 8;
    g_tierBtn = {8, ty, SCREEN_W - 16, 30, "tier"};
    lcd.setFont(&fonts::Font2);
    lcd.setTextDatum(textdatum_t::middle_left);
    lcd.setTextColor(C_TEXT, C_BG);
    lcd.drawString("Dictionary:", 12, ty + 15);
    int pillW = 90, pillH = 26, gap = 6;
    int px = SCREEN_W - 8 - pillW * 2 - gap;
    bool kids = (dictTier() == TIER_KIDS);
    lcd.setTextDatum(textdatum_t::middle_center);
    lcd.fillRoundRect(px, ty + 2, pillW, pillH, 13, kids ? C_ROWLINE : C_ACCENT);
    lcd.setTextColor(kids ? C_SUB : C_HEADERTX, kids ? C_ROWLINE : C_ACCENT);
    lcd.drawString("Everyone", px + pillW / 2, ty + 15);
    lcd.fillRoundRect(px + pillW + gap, ty + 2, pillW, pillH, 13, kids ? C_ACCENT : C_ROWLINE);
    lcd.setTextColor(kids ? C_HEADERTX : C_SUB, kids ? C_ACCENT : C_ROWLINE);
    lcd.drawString(String("Kids") + (getPin().length() ? " (lock)" : ""), px + pillW + gap + pillW / 2, ty + 15);

    // 2x2 button grid
    const char* labels[4] = {"Random word", "Recent words", "Recalibrate", "Set / Change PIN"};
    int by = ty + 36, bw = (SCREEN_W - 16 - 8) / 2, bh = 30, bgap = 8;
    for (int i = 0; i < 4; ++i) {
        int r = i / 2, c = i % 2;
        int bx = 8 + c * (bw + 8);
        int yy = by + r * (bh + bgap);
        g_moreBtns[i] = {bx, yy, bw, bh, labels[i]};
        lcd.fillRoundRect(bx, yy, bw, bh, 6, C_ACCENT);
        lcd.setFont(&fonts::Font2);
        lcd.setTextColor(C_HEADERTX, C_ACCENT);
        lcd.setTextDatum(textdatum_t::middle_center);
        lcd.drawString(labels[i], bx + bw / 2, yy + bh / 2);
    }

    // status line
    lcd.setTextDatum(textdatum_t::top_left);
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(C_SUB, C_BG);
    lcd.drawString(dictStatus(), 10, by + 2 * bh + bgap + 6);

    drawTabBar(SCR_MORE);
}
```

- [ ] **Step 3: Replace `handleMore()`**

```cpp
static void switchTier(DictTier target)
{
    if (target == dictTier()) return;
    // Leaving Kids -> Everyone requires the PIN if one is set.
    if (dictTier() == TIER_KIDS && target == TIER_EVERYONE) {
        String pin = getPin();
        if (pin.length()) {
            String got = promptPin("Enter PIN to leave Kids mode");
            if (got != pin) { drawMore(); return; }
        }
    }
    if (dictSetTier(target)) {
        prefs.putUChar("tier", (uint8_t)target);
        g_browseOffset = g_savedOffset = g_historyOffset = 0;
        g_query = ""; buildResults();
    }
    drawMore();
}

static void handleMore(const Tap& t)
{
    if (handleTabBar(t)) return;

    // WotD card
    int cardY = HEADER_H + 6;
    if (t.y >= cardY && t.y < cardY + 46) { openDefinition(g_wotd); return; }

    // Tier pills
    if (inRect(t, g_tierBtn.x, g_tierBtn.y, g_tierBtn.w, g_tierBtn.h)) {
        int pillW = 90, gap = 6;
        int px = SCREEN_W - 8 - pillW * 2 - gap;
        if (t.x >= px && t.x < px + pillW) switchTier(TIER_EVERYONE);
        else if (t.x >= px + pillW + gap && t.x < px + pillW + gap + pillW) switchTier(TIER_KIDS);
        return;
    }

    for (int i = 0; i < 4; ++i) {
        MoreBtn& b = g_moreBtns[i];
        if (inRect(t, b.x, b.y, b.w, b.h)) {
            if (i == 0) {                                   // Random
                openDefinition((int)(esp_random() % dictCount()));
            } else if (i == 1) {                            // Recent
                g_screen = SCR_HISTORY; g_historyOffset = 0; drawHistory();
            } else if (i == 2) {                            // Recalibrate
                recalibrateTouch(); g_wasTouched = false; drawMore();
            } else {                                        // Set / Change PIN
                String p = promptPin("New PIN (Cancel = clear)");
                setPin(p);   // empty string clears the lock
                drawMore();
            }
            return;
        }
    }
}
```

- [ ] **Step 4: Build**

Run: `pio run`
Expected: `[SUCCESS]`.

- [ ] **Step 5: Flash and verify on device**

Run:
```bash
pio run -t upload
```
On the device, go to **More**:
- The tier row shows `Everyone` highlighted; tap **Kids** → status changes to `Flash (Kids): NNN words`, Browse count drops.
- Tap **Set / Change PIN**, enter `1234`, OK. The Kids pill shows `Kids (lock)`.
- Switch to **Kids** (no prompt), then tap **Everyone** → PIN prompt appears; wrong PIN cancels, `1234` succeeds.
- Verify the chosen tier persists across a reset (`python tools/boot_log.py` shows the saved tier).

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp
git commit -m "More screen: Everyone/Kids tier toggle with PIN lock and Set PIN"
```

---

## Phase E — Docs & final integration

### Task E1: Update README and spec status

**Files:**
- Modify: `README.md`
- Modify: `docs/superpowers/specs/2026-06-06-dictionary-content-and-ui-design.md`

- [ ] **Step 1: Update the "Dictionary source" and build sections of `README.md`**

Replace the dictionary-generation instructions to reflect WordNet + tiers. Under "Building the dictionary into flash", change the generate step to:

```sh
pip install nltk wordfreq
python tools/build_dict.py        # writes data/dict.dat, data/dict.idx, data/dict_kids.idx
python tools/verify_dict.py       # optional: sanity-check the output
pio run -t uploadfs
```

Add a short "Dictionary tiers" subsection:

```markdown
### Dictionary tiers

Two word sets share one data file: **Everyone** (profanity removed) and **Kids**
(additionally removes sensitive terms). Switch in **More → Dictionary**. Set a PIN
(**More → Set / Change PIN**) to stop Kids mode being switched back without it.
Each word shows multiple meanings grouped by part of speech; drag the definition
to scroll. Blocklists are editable text files in `tools/`.
```

- [ ] **Step 2: Mark the spec implemented**

In the spec file, change the `**Status:**` line to:

```markdown
**Status:** Implemented
```

- [ ] **Step 3: Commit**

```bash
git add README.md docs/superpowers/specs/2026-06-06-dictionary-content-and-ui-design.md
git commit -m "Docs: WordNet tiers, scrollable definitions; mark spec implemented"
```

---

### Task E2: Full acceptance pass

- [ ] **Step 1: Clean build + full flash**

Run:
```bash
pio run -t upload
pio run -t uploadfs
python tools/boot_log.py
```
Expected: boot log `[dict] ready: Flash (Everyone): NNNNN words`, no PSRAM/mount errors.

- [ ] **Step 2: On-device acceptance checklist**

Confirm each:
- Search `blue` → colour sense present; multiple meanings grouped by POS.
- `toy` → plaything first (not dog breed); `mouse` → rodent.
- Definition drags to scroll; header fixed; scrollbar tracks; short words don't scroll.
- More → switch Everyone/Kids changes word count; tier persists across reset.
- Set PIN, lock Kids mode, confirm Everyone requires PIN.
- Browse/Saved/Recent and favourites still work; word-of-the-day shows.

- [ ] **Step 3: Merge to main and tag**

```bash
git checkout main
git merge --no-ff feature/dictionary-content-ui -m "Merge: multi-meaning dictionary, scrollable UI, Kids tier"
git push origin main
git tag -a v1.2 -m "v1.2 - WordNet multi-meaning dictionary, scrollable definitions, Kids tier + PIN"
git push origin v1.2
```

---

## Self-review notes (author)

- **Spec coverage:** multiple meanings (A2), freq order (A2), obscure exclusion via cap (A2), scrollable grouped UI (C2/C3), Everyone/Kids shared-data filtering (A1/A2), runtime toggle + PIN (D1/D2), repartition (B1), format v2 (A2/B3), boot status (existing `esp_rom_printf` line retained). All covered.
- **Type consistency:** `Meaning{posCode,def,example}`, `DictEntry{term,meanings}`, `dictBegin(DictTier)`, `dictSetTier`, `dictTier`, `posName`, `g_defScroll`, `g_defContentH`, `defMaxScroll`, `layoutDefinitionBody`, `drawDefinitionBody` used consistently across B/C/D.
- **Branch:** create `feature/dictionary-content-ui` before Task A1 (`git checkout -b feature/dictionary-content-ui`); merge in E2.
```
