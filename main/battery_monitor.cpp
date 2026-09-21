#include "battery_monitor.h"

#include <atomic>

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace BatteryMonitor {
namespace {

constexpr uint8_t kAxp2101Address = 0x34;
constexpr uint8_t kRegStatus1 = 0x00;
constexpr uint8_t kRegStatus2 = 0x01;
constexpr uint8_t kRegPowerOffEnable = 0x22;
constexpr uint8_t kRegPowerKeyLevel = 0x27;
constexpr uint8_t kRegIrqEnable2 = 0x41;
constexpr uint8_t kRegIrqStatus2 = 0x49;
constexpr uint8_t kRegBatteryPercent = 0xA4;

// AXP2101 IRQ2: POWERON Long PRESS IRQ = bit 10 globally = bit 2 in INTEN2/INTSTS2.
constexpr uint8_t kPowerKeyLongIrqMask = 1u << 2;

// AXP2101 register 0x27 fields, per the XPowers implementation bundled by
// Waveshare: IRQLEVEL bits 5:4, OFFLEVEL bits 3:2, ONLEVEL bits 1:0.
constexpr uint8_t kIrqLevel2Seconds = 2u;   // 0=1s, 1=1.5s, 2=2s, 3=2.5s
constexpr uint8_t kOffLevel6Seconds = 1u;   // 0=4s, 1=6s, 2=8s, 3=10s
constexpr uint8_t kOnLevel2Seconds = 3u;    // 0=128ms, 1=512ms, 2=1s, 3=2s

constexpr uint32_t kI2cSpeedHz = 400000;
constexpr int kI2cTimeoutMs = 100;
constexpr TickType_t kKeyPollPeriod = pdMS_TO_TICKS(250);
constexpr uint32_t kTelemetryEveryPolls = 120; // 250 ms * 120 = 30 s

const char *TAG = "BATTERY";
i2c_master_dev_handle_t g_pmu = nullptr;
std::atomic<bool> g_available{false};
std::atomic<bool> g_battery_present{false};
std::atomic<bool> g_vbus_present{false};
std::atomic<bool> g_charging{false};
std::atomic<uint8_t> g_percent{0};
std::atomic<uint32_t> g_power_key_events{0};
std::atomic<bool> g_power_key_ready{false};

esp_err_t read_reg(uint8_t reg, uint8_t &value) {
    if (!g_pmu) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(g_pmu, &reg, 1, &value, 1, kI2cTimeoutMs);
}

esp_err_t write_reg(uint8_t reg, uint8_t value) {
    if (!g_pmu) return ESP_ERR_INVALID_STATE;
    const uint8_t data[2] = {reg, value};
    return i2c_master_transmit(g_pmu, data, sizeof(data), kI2cTimeoutMs);
}

bool update_reg(uint8_t reg, uint8_t clear_mask, uint8_t set_mask) {
    uint8_t value = 0;
    if (read_reg(reg, value) != ESP_OK) return false;
    value = static_cast<uint8_t>((value & static_cast<uint8_t>(~clear_mask)) | set_mask);
    return write_reg(reg, value) == ESP_OK;
}

bool configure_power_key() {
    // Configure the AXP2101's own POWERON key logic rather than assuming a
    // direct ESP32 GPIO. 2 s raises a latched PMU long-press IRQ; keeping the
    // same press held for 6 s lets the PMIC itself remove system power.
    const uint8_t level_clear = 0x3Fu;  // clear IRQLEVEL/OFFLEVEL/ONLEVEL only
    const uint8_t level_set = static_cast<uint8_t>(
        (kIrqLevel2Seconds << 4) |
        (kOffLevel6Seconds << 2) |
        kOnLevel2Seconds);
    if (!update_reg(kRegPowerKeyLevel, level_clear, level_set)) {
        ESP_LOGW(TAG, "AXP2101 power-key timing configuration failed");
        return false;
    }

    // REG 0x22 bit1: enable long-press shutdown.
    // REG 0x22 bit0: 0 = power off, 1 = restart after long press.
    if (!update_reg(kRegPowerOffEnable, 0x03u, 0x02u)) {
        ESP_LOGW(TAG, "AXP2101 long-press shutdown configuration failed");
        return false;
    }

    // Preserve all existing PMU interrupt enables and add POWERON long press.
    if (!update_reg(kRegIrqEnable2, 0x00u, kPowerKeyLongIrqMask)) {
        ESP_LOGW(TAG, "AXP2101 long-press IRQ enable failed");
        return false;
    }

    // AXP2101 interrupt status is write-1-to-clear. Clear only the long-press
    // bit so a stale event cannot immediately shut down the reader.
    if (write_reg(kRegIrqStatus2, kPowerKeyLongIrqMask) != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 stale long-press IRQ clear failed");
        return false;
    }

    ESP_LOGI(TAG, "PWR key configured: shutdown IRQ=2s, HW off=6s, HW on=2s");
    return true;
}

void poll_telemetry() {
    uint8_t status1 = 0;
    uint8_t status2 = 0;
    uint8_t percent = 0;

    const esp_err_t s1 = read_reg(kRegStatus1, status1);
    const esp_err_t s2 = read_reg(kRegStatus2, status2);
    const esp_err_t sp = read_reg(kRegBatteryPercent, percent);
    if (s1 != ESP_OK || s2 != ESP_OK || sp != ESP_OK) {
        g_available.store(false, std::memory_order_relaxed);
        return;
    }

    const bool battery_present = (status1 & (1u << 3)) != 0;
    const bool vbus_present = (status1 & (1u << 5)) != 0;
    const uint8_t direction = static_cast<uint8_t>((status2 >> 5) & 0x03u);
    const bool charging = direction == 0x01u;

    const bool percentage_valid = percent <= 100;
    g_battery_present.store(battery_present, std::memory_order_relaxed);
    g_vbus_present.store(vbus_present, std::memory_order_relaxed);
    g_charging.store(charging, std::memory_order_relaxed);
    g_percent.store(percentage_valid ? percent : 0, std::memory_order_relaxed);
    g_available.store(percentage_valid, std::memory_order_relaxed);
}

void poll_power_key() {
    if (!g_power_key_ready.load(std::memory_order_relaxed)) return;

    uint8_t status = 0;
    if (read_reg(kRegIrqStatus2, status) != ESP_OK) return;
    if ((status & kPowerKeyLongIrqMask) == 0) return;

    if (write_reg(kRegIrqStatus2, kPowerKeyLongIrqMask) == ESP_OK) {
        g_power_key_events.fetch_add(1, std::memory_order_release);
        ESP_LOGI(TAG, "PWR key long-press IRQ (2s)");
    }
}

void battery_task(void *) {
    uint32_t polls = 0;
    while (true) {
        poll_power_key();
        if (++polls >= kTelemetryEveryPolls) {
            polls = 0;
            poll_telemetry();
        }
        vTaskDelay(kKeyPollPeriod);
    }
}

}  // namespace

bool start() {
    if (g_pmu) return true;

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_LOGW(TAG, "BSP I2C bus unavailable");
        return false;
    }

    i2c_device_config_t cfg{};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = kAxp2101Address;
    cfg.scl_speed_hz = kI2cSpeedHz;

    const esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &g_pmu);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 add-device failed: %s", esp_err_to_name(err));
        g_pmu = nullptr;
        return false;
    }

    poll_telemetry();
    g_power_key_ready.store(configure_power_key(), std::memory_order_release);

    const BaseType_t task_result = xTaskCreatePinnedToCore(
        battery_task, "battery_pwr", 3584, nullptr, 2, nullptr, 0);
    if (task_result != pdPASS) {
        ESP_LOGW(TAG, "Could not create battery/PWR task");
        return false;
    }

    const Status s = status();
    ESP_LOGI(TAG, "AXP2101 monitor started: battery=%d vbus=%d charge=%d percent=%u pwr=%d",
             s.battery_present, s.vbus_present, s.charging, s.percent,
             static_cast<int>(power_key_ready()));
    return true;
}

Status status() {
    Status s;
    s.available = g_available.load(std::memory_order_relaxed);
    s.battery_present = g_battery_present.load(std::memory_order_relaxed);
    s.vbus_present = g_vbus_present.load(std::memory_order_relaxed);
    s.charging = g_charging.load(std::memory_order_relaxed);
    s.percent = g_percent.load(std::memory_order_relaxed);
    return s;
}

bool consume_power_key_long_press() {
    uint32_t current = g_power_key_events.load(std::memory_order_acquire);
    while (current > 0) {
        if (g_power_key_events.compare_exchange_weak(
                current, current - 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

bool shutdown() {
    // AXP2101 COMMON_CONFIG (0x10), bit 0: shut down system power outputs.
    // Preserve all other configuration bits, regulator and charger settings.
    return update_reg(0x10, 0x00, 0x01);
}

bool power_key_ready() {
    return g_power_key_ready.load(std::memory_order_acquire);
}

}  // namespace BatteryMonitor
