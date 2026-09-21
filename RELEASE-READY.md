# Open-Source Release Readiness

Status: **READY FOR PUBLIC SOURCE RELEASE**

This status covers the source archive as packaged here. It is not a legal opinion and does not replace compliance review required by a distributor or organization.

## Completed checks

- `LICENSE` contains the retained MIT notice for RSVP Nano plus the notice for Simple RSVP Reader modifications.
- `README.md` and `NOTICE.md` identify RSVP Nano as the upstream project and clearly state that Simple RSVP Reader is an independent port/adaptation, not an official RSVP Nano release.
- `THIRD_PARTY_NOTICES.md` documents RSVP Nano, Waveshare BSP 2.0.3, ESP-IDF, LVGL and Noto Sans.
- The provenance-unknown inherited Unicode bitmap data has been removed.
- `main/unicode_fonts.c` is reproducibly generated from the vendored Noto Sans Regular 2.004 font.
- The complete SIL Open Font License 1.1 is included in `licenses/NotoSans-OFL-1.1.txt`.
- The source font SHA-256 and generation process are documented in `FONT_PROVENANCE.md`.
- No third-party photographs, book covers or complete example e-books are intentionally bundled.
- `sdcard/sample.txt` is a short technical sample.
- Build dependencies are referenced rather than vendored, except for the intentionally vendored OFL font source used to make font generation reproducible.

## Release publisher checklist

Before tagging/publishing a later release, repeat these mechanical checks:

1. Clean build using the intended ESP-IDF version and inspect the resolved component dependency metadata.
2. Confirm any newly added asset/code has an identified compatible license.
3. Regenerate `main/unicode_fonts.c` if the font source, glyph set, Pillow behavior or generator changes.
4. Do not demonstrate copyrighted books unless redistribution/display permission exists; prefer self-authored, public-domain or appropriately licensed content.
5. Keep `LICENSE`, `NOTICE.md`, `THIRD_PARTY_NOTICES.md`, `FONT_PROVENANCE.md` and `licenses/NotoSans-OFL-1.1.txt` in distributed source archives.

## Hardware validation status

Release readiness here is a source/licensing/package assessment. The RC16 PWR implementation still requires validation on the physical Waveshare ESP32-S3 Touch AMOLED 1.8 V2 hardware as described in `PWR-TEST-CHECKLIST.md`. No hardware test is claimed by this document.
