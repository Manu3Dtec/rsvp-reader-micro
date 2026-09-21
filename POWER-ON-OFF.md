# PWR On/Off und Energiepruefung

Stand: 2026-09-16. Gezielte Aenderungen in main/main.cpp und battery_monitor.cpp/.h.
Originalquellen: backup-before-power-off/main/.

- Lockscreen und Standby entfernt.
- PWR-Langdruck: AXP2101-Ereignis nach 2 s, Leseposition und Einstellungen speichern, WLAN geordnet beenden, OFF ca. 650-900 ms anzeigen, PMU-Systemversorgung ueber Register 0x10 Bit 0 abschalten.
- Einschalten: vorhandene AXP2101-Einstellung 2 s beibehalten; ON kurz anzeigen.
- Hardware-Abschaltung nach 6 s bleibt als Rueckfallebene erhalten. Hardware kann keinen 2-s-Abschaltwert einstellen; die 2-s-Abschaltung erfolgt ueber die Firmware.
- BOOT/GPIO0: entprellter kurzer Druck (unter 2 s), Aktion beim Loslassen, Hauptmenue ueber vorhandenen home_cb. Beim Buchladen wird die Menueaktion unterdrueckt, um die Hintergrundarbeit nicht zu stoeren; ein PWR-Abschaltwunsch wird bis zum Abschluss vorgemerkt.
- Die bisherige automatische Display-Abschaltzeit schaltet jetzt das ganze Geraet aus. Der gespeicherte Zeitwert bleibt erhalten, waehrend Wiedergabe, Buchladen und WLAN-Transfer wird nicht automatisch abgeschaltet.

Energiepruefung:
- CONFIG_PM_ENABLE ist im gebauten Projekt aktiviert. DFS 80-240 MHz vorhanden, automatischer Light-Sleep aus zur Peripheriestabilitaet.
- WLAN wird nach Transfer gestoppt und deinitialisiert.
- Akkuabfrage 30 s, Akkuanzeige 15 s, Uhr 60 s bereits sparsam.
- Bisheriger Standby liess MCU/PMU aktiv; dessen Verbrauch wird durch echtes Ausschalten beseitigt.
- Ladeparameter und Regler-Spannungen/-Freigaben werden von der Anwendung nicht konfiguriert und wurden nicht veraendert. Ohne Messung am Geraet sind Akkukapazitaet, Ladestrom, tatsaechliche Ladeschlussspannung und aktive Stromaufnahme nicht feststellbar. Eine Laufzeitverbesserung beim aktiven Lesen ist damit nicht nachgewiesen.

Quellen:
- https://github.com/lewisxhe/XPowersLib/blob/master/src/XPowersAXP2101.hpp
- https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8

Am Geraet pruefen (noch nicht durchgefuehrt):
1. Ohne USB: PWR kurz darf nicht ausschalten; PWR >2 s zeigt OFF, Display und System gehen aus. Danach >2 s PWR: ON und Hauptmenue. Mehrfach wiederholen; auch laengeres Halten pruefen.
2. Aus Lesen, Einstellungen, Bibliothek: BOOT kurz -> Hauptmenue, Leseposition bleibt erhalten. BOOT lang loest keine Menueaktion aus.
3. Ausschalten beim Lesen und beim WLAN-Upload pruefen; Dateien und Leseposition nach Neustart pruefen. PWR beim Buchladen pruefen.
4. Automatische Abschaltzeit pruefen, einschliesslich Einstellung Aus; aktive Wiedergabe bleibt an.
5. Mit USB angeschlossen das Abschalt-/Lade-/Neustartverhalten separat pruefen.
6. Vollstaendig laden, USB entfernen, gleichen Text/WPM/Helligkeit nutzen und Laufzeit sowie Akku-Stromaufnahme messen. Ausschaltstrom separat messen, um echte Abschaltung zu bestaetigen.

Build-Verifikation: ESP-IDF 6.0.2 Build und abschliessender Ninja-Build erfolgreich, Firmware 0x185b60 Bytes, 81% der App-Partition frei. Nicht geflasht; Hardwaretests und Laufzeitmessung stehen aus.
