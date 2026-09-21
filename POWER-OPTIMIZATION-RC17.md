# RC17 Power Optimization (1.8 AMOLED)

- Dynamic frequency scaling enabled: 80-240 MHz, automatic light-sleep disabled for peripheral stability.
- Standby brightness capped at 8%; after 30 seconds the AMOLED turns off. Hold PWR for 2 seconds to resume.
- Standby clock refresh reduced from 1 s to 60 s.
- Battery telemetry reduced from 3 s to 30 s; power-key polling runs every 250 ms.
- Battery UI refresh reduced from 3 s to 15 s.
- Wi-Fi teardown behavior retained (stop + deinit after transfer).
- Existing long-press PWR behavior retained.

The standby clock remains visible for 30 seconds. The screen then turns off to save battery; the MCU and PMU remain active for PWR wake. For maximum storage/runtime, use the 6 s hardware power-off.
