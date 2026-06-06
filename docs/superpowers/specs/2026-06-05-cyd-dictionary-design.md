# CYD Touchscreen Dictionary — Design Spec

**Date:** 2026-06-05
**Target:** ESP32-2432S028R "Cheap Yellow Display" (classic 2.8" CYD)
**Sibling project:** [`Esp480x480Dictionary`](../../../../Esp480x480Dictionary) — same data format and
feature set on the Sunton 4.0" 480×480 board. This spec is the canonical brainstorming
output; the sibling tracks the same decisions for the larger screen.

---

## 1. Goal

An offline, child-friendly touchscreen English dictionary. The user searches or browses
for a word and reads its definition(s) on the device with no network required. The
dictionary content is a **swappable resource file** so it can be regenerated or replaced
(e.g. with a curated child-specific word set) without changing firmware.

## 2. Hardware

| Component | Detail |
|---|---|
| MCU | ESP32-WROOM-32, dual-core 240MHz, WiFi + BT |
| Flash | 4MB (no PSRAM by default) |
| RAM | ~520KB SRAM, ~200KB usable heap after WiFi + Arduino |
| Display | 2.8" 320×240 TFT, ILI9341 driver |
| Touch | XPT2046 **resistive** (single-touch, requires calibration) |
| Storage | microSD (SPI) + internal flash |
| Graphics | LovyanGFX 1.2.7 (workspace standard) |
| **Orientation** | **Landscape, 320×240** (LovyanGFX rotation = 1) |

### Verified pin map (CYD ESP32-2432S028R)

Three independent pin groups — **not** one shared bus:

| Peripheral | Bus | Pins |
|---|---|---|
| ILI9341 display | HSPI | SCLK 14, MOSI 13, MISO 12, CS 15, DC 2, RST -1 (sw), BL 21 |
| XPT2046 touch | own SPI pins | CLK 25, CS 33, MOSI 32, MISO 39, IRQ 36 |
| microSD | VSPI | SCK 18, MOSI 23, MISO 19, CS 5 |

> ⚠️ **Verify against the physical unit before flashing.** CYD board revisions
> (e.g. "R2"/cap-touch variants) differ in touch wiring. Cross-check with the
> [rzeldent Sunton board definitions](https://github.com/rzeldent/platformio-espressif32-sunton)
> and the [RandomNerd CYD pinout](https://randomnerdtutorials.com/esp32-cheap-yellow-display-cyd-pinout-esp32-2432s028r/).

**Bus contention:** The display bus (HSPI) is fully independent. Touch and SD use
different pins and are accessed **sequentially from the main loop** (read touch → then do
SD I/O), never from an ISR, so there is no concurrent-access hazard.

## 3. Navigation model — Layout B (search-first + bottom tab bar), landscape

Selected during visual brainstorming and validated at true 1:1 size on the 320×240 screen.

- **Top:** search field. Tapping it focuses input and shows the on-screen keyboard.
- **Body:** live prefix results while typing (~2 rows visible above the keyboard).
- **Bottom tab bar (persistent):** `Search` · `Browse` · `Saved (★)` · `More`.
- **Definition view:** full-screen, scrollable, with a Back control and a ★ toggle.
- **More tab:** Word of the Day, Random word, Search history, re-run touch calibration.

Keyboard is a landscape QWERTY with ~27px keys (chosen over portrait's ~20px keys because
resistive touch and young users need larger targets; typing is the primary action).

## 4. Features

1. **Type-to-search** — incremental prefix matching against the on-disk index.
2. **Browse A–Z** — letter jump bar + scrolling word list.
3. **Definition view** — headword, part-of-speech, definition(s), example sentence.
4. **Word of the Day** — NTP-seeded by date if WiFi is configured; otherwise rotates
   per boot (no RTC on this board — behaviour is honest about that).
5. **Favorites / bookmarks** — starred words, persisted.
6. **Search history** — recent N words, persisted.
7. **Random word** — jump to a random index entry.

## 5. Dictionary data (the load-bearing design — no PSRAM)

### Source

[Wordset Dictionary](https://github.com/wordset/wordset-dictionary): ~63,936 words /
~177,000 meanings, modern human-readable definitions derived from WordNet. Split across
`a.json`…`z.json` + a misc file. Per-word schema:

```json
{
  "word": "curious",
  "wordset_id": "...",
  "meanings": [
    { "id": "...", "def": "eager to learn or know", "speech_part": "adjective",
      "example": "a curious child", "synonyms": ["inquisitive"] }
  ]
}
```

> License: Wordset is "given to the world." Confirm the repo `LICENSE` file and add
> attribution before redistributing the generated data. For personal/device use this is fine.
> Content is intentionally swappable — a curated child-friendly set can replace it later with
> no firmware change, as long as it is run through `build_dict.py`.

### On-device format (the contract)

A host-side `tools/build_dict.py` converts Wordset JSON into two files on the FAT32 SD root:

- **`dict.bin`** — UTF-8 records, **sorted ascending by lowercased headword**, one per line:
  `headword \t pos \t definition [ \t example ] \n`. Multiple senses joined by ` | `.
- **`dict.idx`** — fixed-width binary index, sorted by headword (little-endian):

  ```c
  struct IdxEntry {        // 16 bytes
      char     key[10];    // first 10 chars of headword, lowercased, NUL-padded
      uint32_t offset;     // byte offset into dict.bin
      uint16_t length;     // record length in bytes
  };
  ```
  ~63,936 entries ≈ ~1.0MB on SD.

### Access strategy

- **Sparse in-RAM index:** at boot, load every ~32nd `dict.idx` entry (key + idx position)
  into RAM (~28KB). Every lookup:
  1. binary-search the sparse index in RAM to find the bounding 32-entry window,
  2. one ~512-byte SD read of that `dict.idx` window,
  3. exact match / prefix scan within the window.
  This bounds each keystroke to a single SD seek+read, keeping search responsive without
  holding the full index in RAM.
- **Definition fetch:** seek `dict.bin` at the entry `offset`, read `length` bytes, parse.
- **Prefix scan:** lower-bound in the index, then forward-scan contiguous entries for the
  result list.

### SD with flash fallback

If a microSD card with `dict.bin` + `dict.idx` is present at boot, it is the data source.
Otherwise the firmware falls back to a small (~500 common words) **LittleFS starter set**
embedded in flash, so the device is usable with no card. UI surfaces an "insert SD for the
full dictionary" notice in the fallback state.

## 6. Rendering (RAM-constrained)

~200KB usable heap means a full-screen 320×240×16bpp sprite (~150KB) is **not affordable**.

- Draw directly to the panel for screen transitions and static chrome.
- Use **small partial sprites** only (a single result row, a key-press highlight) for
  flicker-free local redraws.
- Keep one keyboard layout in flash; redraw pressed keys individually.

## 7. First-boot, persistence & error handling

- **Touch calibration screen on first boot** (`LGFX::calibrateTouch`), result stored to NVS
  (Preferences). Re-runnable from the More tab. Without this the resistive panel is unusable.
- **Persistence (NVS or SD):** touch-cal data, favorites, search history. Favorites/history
  may live on SD when present, NVS otherwise.
- **Missing/corrupt SD** → fall back to starter set + notice; never hard-fail.
- **No results / word not found** → friendly empty state.
- **Partition table:** custom `partitions.csv` (~1.9MB app + ~1.4MB LittleFS). The
  workspace-standard `huge_app.csv` leaves no filesystem and must NOT be used here.

## 8. Module architecture

Mirrors the sibling projects' file conventions.

```
src/
  DisplayConfig.hpp    // CYD pin map (section 2)
  LGFX_Setup.hpp       // LovyanGFX: ILI9341 on HSPI + XPT2046 touch, rotation = 1
  Config.h.template    // optional WiFi creds (NTP), tunables; copied to Config.h (git-ignored)
  Dictionary.hpp/.cpp  // SD/LittleFS open, sparse index load, binary search, prefix scan, record parse
  Keyboard.hpp/.cpp    // landscape QWERTY on-screen keyboard widget
  Screens.hpp/.cpp     // Search / Browse / Definition / Saved / More + bottom tab bar router
  Storage.hpp/.cpp     // favorites, history, touch-cal persistence
  main.cpp             // bring-up, calibration gate, event loop, screen routing
tools/
  build_dict.py        // Wordset *.json -> dict.bin + dict.idx (+ starter-set subset for flash)
data/                  // LittleFS starter set (flash fallback)
docs/superpowers/specs/ // this spec
```

## 9. Build & flash

1. Run `tools/build_dict.py` → produces `dict.bin`, `dict.idx`, and the flash starter set.
2. Copy `dict.bin` + `dict.idx` to a FAT32 microSD card; insert into the CYD.
3. Open in VS Code with PlatformIO; set partition table to the custom `partitions.csv`.
4. (Optional) copy `Config.h.template` → `Config.h` and add WiFi creds for NTP-based WotD.
5. Build: `pio run` · Upload: `pio run --target upload` · Upload FS image for starter set.
6. First boot: complete the touch-calibration screen.

## 10. Testing strategy

- **Host-side:** run `build_dict.py` on a Wordset subset; round-trip test that every
  `dict.idx` offset/length resolves to the correct `dict.bin` record; assert sort order.
- **On-device:**
  - Bring-up: display + calibrated touch (reuse a hello-world before app logic).
  - Lookup correctness for known words; prefix-scan boundary cases (start/end of letter).
  - SD-absent → starter-set fallback path.
  - Calibration persistence across reboot.
  - Favorites/history persistence across reboot.

## 11. Out of scope (for now)

- Audio pronunciation (no speaker; would require external I²S DAC).
- Online lookups / definition updates over WiFi (WiFi used only for optional NTP).
- Editing dictionary content on-device (content is regenerated host-side via `build_dict.py`).

## 12. Open items to confirm during implementation

- Exact CYD pin map vs the physical board revision (section 2 warning).
- Wordset `LICENSE` terms + attribution string.
- Final `dict.bin`/`dict.idx` sizes against actual SD card; tune sparse-index stride if needed.

## Sources

- [Wordset Dictionary (GitHub)](https://github.com/wordset/wordset-dictionary)
- [RandomNerd — CYD ESP32-2432S028R pinout](https://randomnerdtutorials.com/esp32-cheap-yellow-display-cyd-pinout-esp32-2432s028r/)
- [rzeldent — PlatformIO Sunton/CYD board definitions](https://github.com/rzeldent/platformio-espressif32-sunton)
- [Mischianti — ESP32-2432S028 pinout & specs](https://mischianti.org/esp32-2432s028-cheap-yellow-display-high-resolution-pinout-datasheet-schema-and-specs/)
