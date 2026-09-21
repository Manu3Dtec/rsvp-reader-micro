# Hardware validation checklist – RC16 PWR

1. Boot normally and confirm serial log contains:
   - `PWR key configured: standby IRQ=2s, HW off=6s, HW on=2s`
   - `AXP2101 monitor started ... pwr=1`
2. Open a book and start RSVP playback.
3. Hold PWR for about 2 seconds, then release:
   - playback must stop
   - standby screen must appear
   - time and battery must be visible
   - touch must do nothing
4. Hold PWR again for about 2 seconds, then release:
   - reader must return paused at the saved position
   - touch gestures must work again
5. From Home, enter standby and wake:
   - wake should return to Home
6. From WLAN Upload, enter standby:
   - Wi-Fi must shut down cleanly before the lockscreen appears
   - no `Failed to allocate priv TX buffer` / SPI DMA errors
7. Hold PWR continuously for at least 6 seconds:
   - at ~2 s the standby screen may appear
   - at ~6 s AXP2101 must remove system power
8. With power off, hold PWR:
   - AXP2101 is configured for its maximum supported cold-start hold time of 2 s
   - verify the board powers on normally
