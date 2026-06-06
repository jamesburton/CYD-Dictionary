# CYD Dictionary

A touchscreen dictionary for the **Freenove ESP32-S3 Display FNK0104** (variant AB):
2.8" 240×320 IPS (ILI9341), FT6336U capacitive touch, 16 MB flash, 8 MB PSRAM,
and a microSD slot (SDMMC). Built with PlatformIO + LovyanGFX.

The UI is search-first (Layout B) with a persistent bottom tab bar:
**Search / Browse / Saved / More**.

- **Search** – on-screen keyboard with key-press popups, tappable suggestion
  chips, a live top-match preview, and dimmed dead-end letters.
- **Browse** – scroll the whole dictionary A–Z.
- **Saved** – your favourites (tap the star on any definition).
- **More** – word of the day, random word, recent words, touch recalibration,
  and the active dictionary source/status.

## Dictionary source

The firmware reads its words from one of two places, picked automatically at boot:

1. **SD card** (preferred) – the full Wordset corpus (~64,600 words) streamed from
   `dict.idx` + `dict.dat` on the card. Terms live in PSRAM for instant search;
   definitions are read on demand.
2. **Embedded set** (fallback) – a ~90-word child-friendly list baked into the
   firmware (`src/WordData.h`), used whenever the SD card or its files are absent.

The active source and word count are shown at the bottom of the **More** screen,
e.g. `SD: 64645 words` or `SD ok, dict.idx missing`.

## Putting the full dictionary on the SD card

1. Generate the files (one-time; needs Python 3):

   ```sh
   git clone --depth 1 https://github.com/wordset/wordset-dictionary.git tools/wordset
   python tools/build_dict.py
   ```

   This writes `sdcard/dict.idx` and `sdcard/dict.dat`.

2. Format the microSD card as **FAT32** and copy **both** files to its **root**:

   ```
   <SD root>/dict.idx
   <SD root>/dict.dat
   ```

3. Insert the card and reboot. The More screen should report `SD: 64645 words`.

### On-disk format

Both files are little-endian (matching the ESP32-S3). See `tools/build_dict.py`
for the authoritative layout.

- `dict.idx` — `"DIDX"` magic, `u32` version, `u32` count, then per entry
  `[u32 dataOffset][term bytes][0x00]`. Terms are lowercase `a-z` only and sorted
  ascending, so the device's `strcmp` binary search matches Python's `sorted()`.
- `dict.dat` — per entry at `dataOffset`:
  `[u8 posLen][pos][u16 defLen][def][u16 exLen][example]`.

Only pure `a-z` single words are included (the keyboard can't type apostrophes,
hyphens, or spaces anyway). Swapping in a different word source is just a matter
of regenerating these two files.

## Build & flash

```sh
pio run                # build
pio run -t upload      # flash (COM5; see platformio.ini)
pio device monitor     # serial (UART0)
```

Touch calibration runs once on first boot and is stored in NVS; re-run it any
time from **More → Recalibrate touch**.

## Hardware pins

See `src/DisplayConfig.hpp` — display (ILI9341 on HSPI), touch (FT6336U on I2C),
and SD (SDMMC 4-bit) pin assignments, taken from Freenove's own FNK0104AB setup.
