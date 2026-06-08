# Publishing & Release Sequencing — Research (2026-06-08)

> Background research for the forward roadmap (one of three research streams; the
> "device/dictionary landscape" and "content sources & capabilities" streams hit a
> transient background-agent failure and will be re-run inline). Captured here so it
> isn't lost in the conversation.

## Most important finding — decide OTA *before* v1.0

You **cannot switch partition schemes over OTA**, so a factory-only v1.0 owner is
stranded on USB/web reflash forever. If OTA firmware updates are ever wanted, ship the
**dual-OTA-capable partition table in v1.0** even if OTA code lands later:
`otadata (8KB) + ota_0 (~2MB) + ota_1 (~2MB) + LittleFS (~12MB)`. That cuts ~2.4MB from
the dictionary partition — confirm the built `data/` image fits under ~12MB first.
Otherwise keep the current single-app (1.5MB app + 14.4MB LittleFS) and accept
USB/web-only reflash.

## End-user distribution
- **ESP Web Tools / esptool-js** "Connect & Install" button on a GitHub Pages site —
  zero toolchain; needs a `manifest.json` (chipFamily `ESP32-S3`) and ideally a single
  **merged bin** (`esptool --chip esp32s3 merge_bin`). Caveat: the ~14MB LittleFS image
  must also be flashed (slow in-browser) — or web-flash firmware only and steer users to
  the **SD-card** data path.
- **GitHub Releases** assets per release: merged bin, app-only bin, `littlefs-data` image,
  raw `dict.dat`/`dict.idx` (SD users). Build via GitHub Actions.
- **Versioning:** firmware SemVer **and a separate dictionary dataset stamp** (e.g.
  `dict-2026.06`) that bumps independently; surface both at boot / More screen.

## OTA + data updates
- Firmware OTA later via ArduinoOTA (LAN), HTTPUpdate/esp32FOTA (pull+version check), or
  ElegantOTA (web upload) — only painless if the OTA partition table shipped in v1.0.
- **Do NOT OTA the ~14MB dictionary.** Update data via (1) **SD card** (already the
  highest-priority source) or (2) a LAN **LittleFS image** push (`Update` supports
  `U_LITTLEFS`). Design dictionaries as overlay-able add-ons (matches the supplementary-
  dictionary feature).

## Licensing (corrects an earlier assumption)
- **WordNet is permissively licensed** (WordNet License, OSI-approved, MIT-style) — NOT
  CC-BY-SA. Commercial use OK, no share-alike. Obligation: include Princeton's copyright
  notice + AS-IS disclaimer in a `NOTICE`/`THIRD-PARTY-LICENSES` file; don't use
  "Princeton" in advertising.
- Host-build tooling: **NLTK** Apache-2.0, **wordfreq** MIT (pin the version — later
  releases changed data sourcing). **LovyanGFX** BSD-2-Clause (include notice).
- Keep **code license** (MIT/Apache-2.0) separate from **bundled data** (WordNet + NOTICE).
- Add a **"filtering is best-effort, parental discretion advised"** disclaimer.

## Visibility checklist
- Submit to witnessmenow `ESP32-Cheap-Yellow-Display/PROJECTS.md` — but label honestly as
  a **Freenove ESP32-S3 / "CYD-adjacent"** board (not the ESP32-2432S028R).
- r/esp32, CYD Discord, PlatformIO Community, Hackster writeup, pitch Hackaday.
- Docs site (GitHub Pages, optionally MkDocs Material) hosting the web-flasher + docs +
  screenshots. 30–90s demo video (type-to-search, definition, tier/PIN).
- Repo polish: screenshots/GIF, topics/tags, pinned Releases link, web-flash button.

## Proposed release sequence
- **v1.0 "flash it easily":** current features + ESP Web Tools installer + GitHub Release
  assets + LICENSE/NOTICE/CONTRIBUTING + demo video. **Adopt OTA partition layout now even
  if OTA is off** (the gating decision). Dataset stamp `dict-2026.06`.
- **v1.1–v1.x data & polish:** supplementary/add-on dictionaries via SD (+ optional LAN
  LittleFS push), curated child word sets, UX fixes. Bump dataset version independently.
- **v2.0 OTA + big capabilities:** enable app-OTA (painless thanks to v1.0 table), plus
  **ES8311 audio pronunciation**, images/illustrations, WiFi online-lookup augmentation.

_Sources: ESP Web Tools, ESP-IDF partition/OTA docs, arduino-esp32 OTA (#7267 U_LITTLEFS),
ElegantOTA, esp32FOTA, Princeton/OSI WordNet license, NLTK, wordfreq, witnessmenow CYD repo,
Hackaday CYD coverage, r0b.io GH-Actions+SPIFFS. (Full URLs in the research transcript.)_
