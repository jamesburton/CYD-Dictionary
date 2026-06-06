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

The packed dictionary is only ~5.1 MB, so it lives in the device's own 16 MB
flash — no SD card needed. The firmware picks a source automatically at boot, in
priority order:

1. **SD card** (optional override) – if a card holds `dict.idx` + `dict.dat`, it
   wins, letting you swap dictionaries without reflashing.
2. **Flash / LittleFS** (built-in default) – the full Wordset corpus (~64,600
   words) flashed into the device. This is the normal source.
3. **Embedded set** (last resort) – a ~90-word child-friendly list baked into the
   firmware (`src/WordData.h`), used only if both above are unavailable.

In every case the 864 KB index is loaded into PSRAM for instant prefix search;
definitions are streamed from the source on demand. The active source and word
count are shown at the bottom of the **More** screen (e.g. `Flash: 64645 words`),
and printed at boot over the USB console (`[dict] ready: Flash: 64645 words`).

## Building the dictionary into flash (default)

1. Generate the files (one-time; needs Python 3):

   ```sh
   git clone --depth 1 https://github.com/wordset/wordset-dictionary.git tools/wordset
   python tools/build_dict.py
   ```

   This writes `data/dict.idx` and `data/dict.dat` (`data/` is PlatformIO's
   filesystem-image source directory).

2. Flash the dictionary image into the device's LittleFS partition:

   ```sh
   pio run -t uploadfs
   ```

3. Reboot. The More screen should report `Flash: 64645 words`.

### Optional: overriding from an SD card

To swap in a different dictionary without reflashing, copy the same two files to
the **root** of a **FAT32** microSD card and insert it:

```
<SD root>/dict.idx
<SD root>/dict.dat
```

A card with a valid dictionary takes priority over the built-in flash copy; the
More screen will then read `SD: 64645 words`.

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
pio run                # build firmware
pio run -t upload      # flash firmware (COM5; see platformio.ini)
pio run -t uploadfs    # flash the dictionary into LittleFS (see above)
```

> **Serial note:** `platformio.ini` sets `ARDUINO_USB_CDC_ON_BOOT=0` so the COM
> port stays stable for flashing. As a result Arduino `Serial` is on UART0
> (physical pins), not USB — only ROM/ESP-IDF logs (and the `esp_rom_printf`
> boot line) appear over the USB console.

Touch calibration runs once on first boot and is stored in NVS; re-run it any
time from **More → Recalibrate touch**.

## Hardware pins

See `src/DisplayConfig.hpp` — display (ILI9341 on HSPI), touch (FT6336U on I2C),
and SD (SDMMC 4-bit) pin assignments, taken from Freenove's own FNK0104AB setup.
