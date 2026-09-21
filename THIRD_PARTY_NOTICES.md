# Third-Party Notices

This file documents the principal third-party projects used by Simple RSVP Reader. Dependencies fetched by ESP-IDF's component manager retain their own copyright notices and license files. Those upstream notices remain controlling for the corresponding components.

## RSVP Nano

- Project: RSVP Nano
- Upstream: https://github.com/ionutdecebal/rsvpnano
- License: MIT License
- Copyright: Copyright (c) 2026 RSVP Nano contributors
- Relationship: Simple RSVP Reader is an independent port/adaptation derived in part from RSVP Nano; it is not an official RSVP Nano release.
- The complete retained MIT text is in this repository's `LICENSE` file.

## Waveshare ESP32-S3 Touch AMOLED 1.8 BSP

- Component: `waveshare/esp32_s3_touch_amoled_1_8`
- Version pinned by this project: `2.0.3`
- Source/distribution: ESP Component Registry
- Component page: https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_1_8/versions/2.0.3/readme
- License page: https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_1_8/versions/2.0.3/license
- License: Apache License 2.0

The BSP is downloaded as a build dependency and is not copied into this source archive. Preserve the license/notice material delivered with the resolved component when redistributing a source bundle that vendors dependencies or when redistributing component source.

## ESP-IDF

- Project: Espressif IoT Development Framework (ESP-IDF)
- Expected build version for this firmware: 6.0.2
- Upstream: https://github.com/espressif/esp-idf
- License: Apache License 2.0

ESP-IDF is a build/framework dependency and is not copied into this source archive.

## LVGL

- Project: LVGL (Light and Versatile Graphics Library)
- Upstream: https://github.com/lvgl/lvgl
- License: MIT License
- Relationship: Used through the ESP-IDF/Waveshare component dependency graph.

LVGL and its fetched/bundled dependencies retain their upstream notices and license files.

## Noto Sans embedded Unicode font subset

- Source font: Noto Sans Regular 2.004
- Vendored source: `tools/font/NotoSans-Regular.ttf`
- Font SHA-256: `89c3c497f618fdaa0b2d1e98fef93582f28c71debd2c4a8cdf41f190ced2909d`
- Upstream project: https://github.com/notofonts/latin-greek-cyrillic
- License: SIL Open Font License 1.1
- Full license: `licenses/NotoSans-OFL-1.1.txt`
- Generated firmware data: `main/unicode_fonts.c`
- Reproducible generator: `tools/font/generate_unicode_fonts.py`

The inherited provenance-unknown bitmap data was removed and replaced by a reproducibly generated subset from the identified OFL-licensed source font. The generated font material remains subject to the SIL OFL 1.1; it is not relicensed under the project's MIT license. See `FONT_PROVENANCE.md`.

## Build-time dependency review

The exact transitive dependency set can change when component-manager constraints resolve differently. Before publishing a release, perform a clean build with the intended ESP-IDF version and review the resolved component lock/dependency metadata and license files. Any additional third-party component must retain its own applicable notices.
