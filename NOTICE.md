# NOTICE - Simple RSVP Reader

## Origin and project relationship

Simple RSVP Reader is an independent open-source port and adaptation inspired by and derived in part from RSVP Nano. It is not an official RSVP Nano release.

Original project: RSVP Nano  
Original repository: https://github.com/ionutdecebal/rsvpnano  
Original license: MIT License  
Original copyright notice: Copyright (c) 2026 RSVP Nano contributors

The original RSVP Nano copyright and MIT license notice are retained in this repository. Modifications and porting work in this distribution are additionally identified as Copyright (c) 2026 Simple RSVP Reader contributors and are distributed under the MIT License unless a file or third-party component states otherwise.

## Deutsche Kennzeichnung

Simple RSVP Reader ist eine unabhängige Open-Source-Portierung und Weiterentwicklung, die teilweise auf RSVP Nano basiert. Sie ist keine offizielle Veröffentlichung des ursprünglichen Projekts. Der ursprüngliche Copyright- und Lizenzhinweis der MIT-Lizenz bleibt erhalten.

## Main changes in this port

This branch/port targets the Waveshare ESP32-S3 Touch AMOLED 1.8 V2 and uses ESP-IDF rather than representing an official RSVP Nano release. It contains board-specific display/touch/power integration, a reader/UI adapted to the 368x448 display, EPUB/TXT handling, SD-card operation, Wi-Fi upload functionality, RTC/battery integration and the RC16 PWR standby/lockscreen implementation.

For detailed release history, see the RC*.md files in this archive.

## Media and demonstration content

No third-party photos, book covers or complete example e-books are intentionally bundled with this source archive. `sdcard/sample.txt` is a short technical sample.

When publishing screenshots or videos, use self-created hardware/UI media and only self-authored, public-domain, or appropriately licensed text/book content.

## Embedded font

The previously undocumented inherited font bitmap has been replaced by a reproducibly generated subset of Noto Sans Regular 2.004. The source TTF, generator, provenance record and SIL Open Font License 1.1 are included in this repository. See `FONT_PROVENANCE.md` and `licenses/NotoSans-OFL-1.1.txt`.
