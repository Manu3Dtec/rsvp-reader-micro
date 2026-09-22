<p align="center"><img src="docs/assets/logo.png" width="520" alt="Simple RSVP Reader logo"></p>

# Simple RSVP Reader Micro

Open-source RSVP speed-reading firmware for the Waveshare ESP32-S3 Touch AMOLED 1.8 V2 (CO5300/CST816S), built with ESP-IDF 6.0.2.

## Install without development tools

**[Open the browser installer](https://manu3dtec.github.io/rsvp-reader-micro/installer/)** using desktop Chrome or Edge. Connect the board by USB, click **Connect & install**, select its serial port, and follow the prompts. No ESP-IDF setup or command line is required.

The installer files are stored in `docs/installer`. Advanced users can still build and flash the project with ESP-IDF using the scripts in this repository.

## Open-source origin and licensing

Simple RSVP Reader is an independent open-source port and adaptation inspired by and derived in part from **RSVP Nano**. It is **not an official RSVP Nano release**.

Original project: https://github.com/ionutdecebal/rsvpnano

RSVP Nano is licensed under the MIT License. The original notice `Copyright (c) 2026 RSVP Nano contributors` and the MIT license text are retained in this repository. See `LICENSE` and `NOTICE.md`.

Third-party dependencies and their licenses are documented in `THIRD_PARTY_NOTICES.md`.

### Embedded font licensing

The provenance-unknown inherited font bitmap has been replaced. `main/unicode_fonts.c` is reproducibly generated from the vendored **Noto Sans Regular 2.004** source font under the **SIL Open Font License 1.1**. The source font, exact SHA-256, generator, glyph range and license are included in `tools/font/`, `FONT_PROVENANCE.md` and `licenses/NotoSans-OFL-1.1.txt`.

This source archive is prepared for public open-source release. Project code is MIT-licensed unless a file or third-party notice states otherwise.


## Controls

- Tap in the reader: start or pause
- Swipe up: increase speed by 50 WPM
- Swipe down: decrease speed by 50 WPM
- Swipe left: next word
- Swipe right: previous word
- Settings: adjust WPM with the -50/+50 buttons

## Wi-Fi upload

Select **WLAN Upload** in the main menu.

- SSID: `RSVP-Reader`
- Password: `reader1234`
- Browser: `http://192.168.4.1`

EPUB and TXT files are written directly to the microSD card. USB file transfer was removed in RC8.

## RC8 touch improvements

- Swipes are completed on `LV_EVENT_PRESS_LOST` instead of being discarded.
- A transparent input layer over the reader captures gestures consistently.
- Menu cards have increased spacing.
- Touches directly on a card edge are ignored to prevent accidental activation of an adjacent entry.
- The WPM -/+ controls use the same robust press-and-release handling.

## Build

```powershell
cd C:\RSVP-Reader-V2
python C:\esp-idf-v6.0.2\tools\idf.py fullclean
python C:\esp-idf-v6.0.2\tools\idf.py set-target esp32s3
python C:\esp-idf-v6.0.2\tools\idf.py build
```

Flash:

```powershell
python C:\esp-idf-v6.0.2\tools\idf.py flash monitor
```

## RC10 input architecture

RC10 uses the Waveshare BSP 2.0.3 input device and LVGL native event routing
directly. The previous custom SafeTap layer has been removed.

Reader controls:
- swipe up: +50 WPM
- swipe down: -50 WPM
- swipe left: next word
- swipe right: previous word
- tap: start/pause

Character display remains robust with ASCII fallbacks for German umlauts and
common European punctuation while the original UTF-8 book remains untouched.
