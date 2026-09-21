RSVP Reader V2 v2.2 RC16 PWR/STANDBY
=====================================
Target: Waveshare ESP32-S3 Touch AMOLED 1.8 V2
Display/Touch: CO5300 / CST820
ESP-IDF: v6.0.2

Wichtigste Funktionen:
- EPUB/TXT von microSD
- RSVP Reader mit ORP-Hervorhebung
- Tap: Start/Pause
- Wisch hoch/runter: +/-25 WPM
- Wisch links/rechts: naechstes/vorheriges Wort
- Fortschritt in Prozent, EPUB zusaetzlich Kapitel x/y
- Letzte Buecher, Buch loeschen, SD-Speicheranzeige
- Helligkeit und Auto-Sleep
- Einstellbare Satzzeichen-Pausen
- Sprache Deutsch / English
- Akkuanzeige + RTC-Uhr
- WLAN-Hotspot Upload: RSVP-Reader / reader1234 / 192.168.4.1
- PWR 2s: Standby-Lockscreen mit Uhr/Akku, Touch aus
- PWR erneut 2s: Reader/Home wiederherstellen, Touch an
- PWR ca. 6s: hardwareseitiges Ausschalten ueber AXP2101
- Einschalten: AXP2101 unterstuetzt maximal 2s PWR-Haltezeit
- Satz- und Sperren-Buttons sind entfernt

Empfohlen: BUILD-FLASH.bat ausfuehren.
Alternativ PowerShell:
  .\build_and_flash.ps1
oder mit festem Port:
  .\build_and_flash.ps1 -Port COM5

Manuell:
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
cd C:\esp-idf-v6.0.2
.\export.ps1
cd C:\RSVP-Reader-V2
python C:\esp-idf-v6.0.2\tools\idf.py fullclean
python C:\esp-idf-v6.0.2\tools\idf.py set-target esp32s3
python C:\esp-idf-v6.0.2\tools\idf.py build
python C:\esp-idf-v6.0.2\tools\idf.py flash monitor
