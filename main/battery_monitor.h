#pragma once

#include <cstdint>

namespace BatteryMonitor {

struct Status {
    bool available = false;
    bool battery_present = false;
    bool vbus_present = false;
    bool charging = false;
    uint8_t percent = 0;
};

// Uses the already initialized Waveshare BSP I2C bus. Besides battery
// telemetry, start() configures only the AXP2101 POWERON-key registers needed
// by the RSVP power-key feature:
//   - 2 s long-press IRQ
//   - 6 s hardware power-off
//   - 2 s hardware power-on (AXP2101 maximum)
// No regulator voltage/enable or charger settings are modified.
bool start();
Status status();

// Returns true once for every latched AXP2101 2-second POWERON long-press IRQ.
// The PMU interrupt status is polled over I2C, so no board-specific IRQ GPIO is
// guessed or required.
bool consume_power_key_long_press();

// True when the AXP2101 POWERON-key configuration was applied successfully.
bool power_key_ready();

// Remove system power through AXP2101; RTC backup supply remains powered.
bool shutdown();

}  // namespace BatteryMonitor
