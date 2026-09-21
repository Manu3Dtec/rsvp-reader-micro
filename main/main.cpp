#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <new>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <vector>

#include "bsp/esp-bsp.h"
#include "battery_monitor.h"
#include "epub_reader.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "reader_engine.h"
#include "transfer_common.h"
#include "transfer_wifi.h"
#include "unicode_fonts.h"
#include "rtc_clock.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr uint32_t kBg = 0x050506;
constexpr uint32_t kCard = 0x141418;
constexpr uint32_t kCardPressed = 0x202027;
constexpr uint32_t kText = 0xF5F5F7;
constexpr uint32_t kMuted = 0x8E8E93;
constexpr uint32_t kAccent = 0x5E5CE6;
constexpr uint32_t kOrp = 0xFF453A;
constexpr uint16_t kMinWpm = 100;
constexpr uint16_t kMaxWpm = 1000;
constexpr uint16_t kWpmStep = 25;

enum class Language : uint8_t { German = 0, English = 1 };
enum class TransferMode : uint8_t { None = 0, Wifi };

const char *TAG = "RSVP_V2";

void configure_power_saving() {
    // Dynamic frequency scaling reduces idle CPU power without putting peripherals
    // into automatic light sleep. Drivers can still request the maximum clock when needed.
    esp_pm_config_t pm{};
    pm.max_freq_mhz = 240;
    pm.min_freq_mhz = 80;
    pm.light_sleep_enable = false;
    const esp_err_t err = esp_pm_configure(&pm);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Power saving: DFS enabled (80-240 MHz, auto light-sleep off)");
    } else {
        ESP_LOGW(TAG, "Power saving setup unavailable: %s", esp_err_to_name(err));
    }
}

ReaderEngine g_reader;
std::vector<std::string> g_books;
std::string g_active_book_key;
std::string g_active_book_title;
std::string g_last_book;
std::vector<std::string> g_recent_books;
std::string g_pending_delete;

lv_obj_t *g_screen = nullptr;
lv_obj_t *g_before = nullptr;
lv_obj_t *g_orp = nullptr;
lv_obj_t *g_after = nullptr;
lv_obj_t *g_progress_label = nullptr;
lv_obj_t *g_speed_label = nullptr;
lv_obj_t *g_play_hint = nullptr;
lv_obj_t *g_progress_bar = nullptr;
lv_obj_t *g_overlay = nullptr;
lv_timer_t *g_reader_timer = nullptr;
lv_timer_t *g_overlay_timer = nullptr;
lv_timer_t *g_transfer_timer = nullptr;
lv_timer_t *g_battery_timer = nullptr;
lv_timer_t *g_clock_timer = nullptr;
lv_timer_t *g_reader_touch_timer = nullptr;
lv_indev_t *g_touch_indev = nullptr;
lv_obj_t *g_transfer_status = nullptr;
lv_obj_t *g_transfer_bar = nullptr;
lv_obj_t *g_battery_text = nullptr;
lv_obj_t *g_battery_fill = nullptr;
lv_obj_t *g_clock_text = nullptr;
lv_obj_t *g_sentence_pause_label = nullptr;
lv_obj_t *g_clause_pause_label = nullptr;
lv_obj_t *g_brightness_label = nullptr;
lv_obj_t *g_sleep_label = nullptr;

bool g_playing = false;
bool g_sd_mounted = false;
bool g_book_loading = false;
uint16_t g_wpm = 350;
uint32_t g_words_since_save = 0;
Language g_language = Language::German;
TransferMode g_transfer_mode = TransferMode::None;
bool g_home_after_transfer_stop = false;
std::atomic<bool> g_transfer_stop_complete{false};
bool g_shutdown_after_transfer_stop = false;
bool g_shutdown_pending = false;
bool g_shutdown_requested = false;
int64_t g_shutdown_deadline_us = 0;
bool g_display_sleeping = false;
uint8_t g_brightness = 40;
uint8_t g_auto_sleep_minutes = 5;
uint16_t g_sentence_pause_pct = 100;
uint16_t g_clause_pause_pct = 40;
int64_t g_last_activity_us = 0;

bool g_reader_mode = false;
bool g_reader_touch_active = false;
bool g_reader_gesture_fired = false;
lv_point_t g_reader_touch_start = {0, 0};
lv_point_t g_reader_touch_last = {0, 0};

const char *tr(const char *de, const char *en) {
    return g_language == Language::English ? en : de;
}

uint32_t fnv1a(const char *s) {
    uint32_t h = 2166136261u;
    while (s && *s) {
        h ^= static_cast<uint8_t>(*s++);
        h *= 16777619u;
    }
    return h;
}

std::string position_key(const std::string &path) {
    char key[16];
    std::snprintf(key, sizeof(key), "p%08lx", static_cast<unsigned long>(fnv1a(path.c_str())));
    return key;
}

bool file_exists(const std::string &path) {
    struct stat st {};
    return !path.empty() && stat(path.c_str(), &st) == 0;
}

std::string basename_no_ext(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name.resize(dot);
    return name;
}

bool has_extension(const std::string &name, const char *wanted) {
    std::string ext = wanted ? wanted : "";
    if (name.size() < ext.size()) return false;
    std::string tail = name.substr(name.size() - ext.size());
    std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return tail == ext;
}

bool is_supported_book(const std::string &name) {
    return has_extension(name, ".epub") || has_extension(name, ".txt");
}

bool remove_file_if_exists(const std::string &path) {
    if (path.empty()) return false;
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) return false;
    return std::remove(path.c_str()) == 0;
}

void touch_activity() {
    g_last_activity_us = esp_timer_get_time();
    if (g_display_sleeping) {
        g_display_sleeping = false;
        bsp_display_brightness_set(g_brightness);
    }
}

std::string format_storage() {
    if (!g_sd_mounted) return tr("microSD nicht erkannt", "microSD not detected");
    struct statvfs fs {};
    if (statvfs(BSP_SD_MOUNT_POINT, &fs) != 0) return tr("Speicher unbekannt", "Storage unknown");
    const uint64_t total = static_cast<uint64_t>(fs.f_blocks) * fs.f_frsize;
    const uint64_t freeb = static_cast<uint64_t>(fs.f_bavail) * fs.f_frsize;
    char out[80];
    const double total_gb = static_cast<double>(total) / (1024.0 * 1024.0 * 1024.0);
    const double free_gb = static_cast<double>(freeb) / (1024.0 * 1024.0 * 1024.0);
    std::snprintf(out, sizeof(out), tr("%.1f GB frei / %.1f GB", "%.1f GB free / %.1f GB"),
                  free_gb, total_gb);
    return out;
}

void remember_recent(const std::string &path) {
    if (path.empty()) return;
    g_recent_books.erase(std::remove(g_recent_books.begin(), g_recent_books.end(), path), g_recent_books.end());
    g_recent_books.insert(g_recent_books.begin(), path);
    if (g_recent_books.size() > 3) g_recent_books.resize(3);
}

void forget_recent(const std::string &path) {
    g_recent_books.erase(std::remove(g_recent_books.begin(), g_recent_books.end(), path), g_recent_books.end());
    if (g_last_book == path) g_last_book.clear();
}


std::string display_safe(std::string text) {
    // RC11 uses Unicode-capable fallback fonts for Latin-1 and common EPUB
    // punctuation. Keep the original UTF-8 text intact.
    return text;
}

void scan_books() {
    g_books.clear();
    if (!g_sd_mounted) return;
    DIR *dir = opendir(BSP_SD_MOUNT_POINT);
    if (!dir) return;
    while (dirent *entry = readdir(dir)) {
        const std::string name(entry->d_name);
        if (name.empty() || name[0] == '.' || !is_supported_book(name)) continue;
        g_books.emplace_back(std::string(BSP_SD_MOUNT_POINT) + "/" + name);
    }
    closedir(dir);
    std::sort(g_books.begin(), g_books.end(), [](const std::string &a, const std::string &b) {
        std::string aa = a;
        std::string bb = b;
        std::transform(aa.begin(), aa.end(), aa.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(bb.begin(), bb.end(), bb.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return aa < bb;
    });
}

size_t load_position(const std::string &path) {
    nvs_handle_t handle;
    if (nvs_open("positions", NVS_READONLY, &handle) != ESP_OK) return 0;
    uint32_t value = 0;
    nvs_get_u32(handle, position_key(path).c_str(), &value);
    nvs_close(handle);
    return value;
}

void save_position() {
    if (!g_reader.valid() || g_active_book_key.empty()) return;
    nvs_handle_t handle;
    if (nvs_open("positions", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u32(handle, position_key(g_active_book_key).c_str(), static_cast<uint32_t>(g_reader.position()));
    nvs_commit(handle);
    nvs_close(handle);
    g_words_since_save = 0;
}

void load_settings() {
    nvs_handle_t handle;
    if (nvs_open("settings", NVS_READONLY, &handle) != ESP_OK) return;
    uint16_t value = g_wpm;
    if (nvs_get_u16(handle, "wpm", &value) == ESP_OK) g_wpm = std::clamp<uint16_t>(value, kMinWpm, kMaxWpm);
    uint8_t lang = 0;
    if (nvs_get_u8(handle, "lang", &lang) == ESP_OK) g_language = lang == 1 ? Language::English : Language::German;

    uint8_t brightness = g_brightness;
    if (nvs_get_u8(handle, "bright", &brightness) == ESP_OK) g_brightness = std::clamp<uint8_t>(brightness, 10, 100);
    uint8_t sleep_min = g_auto_sleep_minutes;
    if (nvs_get_u8(handle, "sleep", &sleep_min) == ESP_OK) g_auto_sleep_minutes = sleep_min;
    uint16_t sentence_pct = g_sentence_pause_pct;
    if (nvs_get_u16(handle, "sent_pct", &sentence_pct) == ESP_OK) g_sentence_pause_pct = std::min<uint16_t>(sentence_pct, 200);
    uint16_t clause_pct = g_clause_pause_pct;
    if (nvs_get_u16(handle, "clause_pct", &clause_pct) == ESP_OK) g_clause_pause_pct = std::min<uint16_t>(clause_pct, 150);

    size_t required = 0;
    if (nvs_get_str(handle, "last_book", nullptr, &required) == ESP_OK && required > 1 && required < 256) {
        std::vector<char> buffer(required, 0);
        if (nvs_get_str(handle, "last_book", buffer.data(), &required) == ESP_OK) g_last_book = buffer.data();
    }
    g_recent_books.clear();
    for (int i = 0; i < 3; ++i) {
        char key[8];
        std::snprintf(key, sizeof(key), "rec%d", i);
        size_t rec_required = 0;
        if (nvs_get_str(handle, key, nullptr, &rec_required) == ESP_OK && rec_required > 1 && rec_required < 256) {
            std::vector<char> buf(rec_required, 0);
            if (nvs_get_str(handle, key, buf.data(), &rec_required) == ESP_OK && file_exists(buf.data())) {
                g_recent_books.emplace_back(buf.data());
            }
        }
    }
    nvs_close(handle);
}

void save_settings() {
    nvs_handle_t handle;
    if (nvs_open("settings", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u16(handle, "wpm", g_wpm);
    nvs_set_u8(handle, "lang", static_cast<uint8_t>(g_language));
    nvs_set_u8(handle, "bright", g_brightness);
    nvs_set_u8(handle, "sleep", g_auto_sleep_minutes);
    nvs_set_u16(handle, "sent_pct", g_sentence_pause_pct);
    nvs_set_u16(handle, "clause_pct", g_clause_pause_pct);
    if (!g_last_book.empty()) nvs_set_str(handle, "last_book", g_last_book.c_str());
    else nvs_erase_key(handle, "last_book");
    for (int i = 0; i < 3; ++i) {
        char key[8];
        std::snprintf(key, sizeof(key), "rec%d", i);
        if (i < static_cast<int>(g_recent_books.size())) nvs_set_str(handle, key, g_recent_books[i].c_str());
        else nvs_erase_key(handle, key);
    }
    nvs_commit(handle);
    nvs_close(handle);
}

void show_home();
void show_library();
void show_settings();
void show_delete_confirm(const std::string &path);
void show_shutdown_ui();
void request_shutdown();
void build_reader_ui();
void stop_reader(bool save);
void create_clock_indicator();

void delete_ui_timers() {
    if (g_reader_timer) {
        lv_timer_delete(g_reader_timer);
        g_reader_timer = nullptr;
    }
    if (g_overlay_timer) {
        lv_timer_delete(g_overlay_timer);
        g_overlay_timer = nullptr;
    }
    if (g_transfer_timer) {
        lv_timer_delete(g_transfer_timer);
        g_transfer_timer = nullptr;
    }
    if (g_battery_timer) {
        lv_timer_delete(g_battery_timer);
        g_battery_timer = nullptr;
    }
    if (g_clock_timer) {
        lv_timer_delete(g_clock_timer);
        g_clock_timer = nullptr;
    }
    if (g_reader_touch_timer) {
        lv_timer_delete(g_reader_touch_timer);
        g_reader_touch_timer = nullptr;
    }
}

void transfer_stop_task(void *arg) {
    const auto mode = static_cast<TransferMode>(reinterpret_cast<intptr_t>(arg));
    ESP_LOGI(TAG, "Transfer stop task: stopping backend");
    if (mode == TransferMode::Wifi) WifiTransfer::stop();
    ESP_LOGI(TAG, "Transfer backend stopped; DMA/internal free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)));
    // Never manipulate LVGL from this worker. The existing LVGL power timer
    // will perform the screen transition on the LVGL thread.
    g_transfer_stop_complete.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}

void stop_active_transfer() {
    const TransferMode mode = g_transfer_mode;
    if (mode == TransferMode::None) return;
    g_transfer_mode = TransferMode::None;
    g_transfer_stop_complete.store(false, std::memory_order_release);
    const BaseType_t ok = xTaskCreatePinnedToCore(
        transfer_stop_task, "transfer_stop", 4096,
        reinterpret_cast<void *>(static_cast<intptr_t>(mode)),
        3, nullptr, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Could not create transfer stop task");
        g_transfer_mode = mode; // allow a retry; do not pretend Wi-Fi stopped
        g_transfer_stop_complete.store(false, std::memory_order_release);
    }
}

void create_battery_indicator();

void clear_screen(bool status_indicators = true) {
    g_reader_mode = false;
    g_reader_touch_active = false;
    g_reader_gesture_fired = false;
    stop_active_transfer();
    delete_ui_timers();
    g_playing = false;

    lv_obj_t *old = g_screen;
    g_screen = lv_obj_create(nullptr);
    lv_obj_remove_flag(g_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(g_screen, lv_color_hex(kBg), 0);
    lv_obj_set_style_bg_opa(g_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(g_screen, lv_color_hex(kText), 0);
    lv_screen_load(g_screen);
    if (old) lv_obj_delete(old);

    g_before = nullptr;
    g_orp = nullptr;
    g_after = nullptr;
    g_progress_label = nullptr;
    g_speed_label = nullptr;
    g_play_hint = nullptr;
    g_progress_bar = nullptr;
    g_overlay = nullptr;
    g_transfer_status = nullptr;
    g_transfer_bar = nullptr;
    g_battery_text = nullptr;
    g_battery_fill = nullptr;
    g_clock_text = nullptr;
    g_sentence_pause_label = nullptr;
    g_clause_pause_label = nullptr;
    g_brightness_label = nullptr;
    g_sleep_label = nullptr;

    if (status_indicators) {
        create_battery_indicator();
        create_clock_indicator();
    }
}

lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    if (font) lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}


void update_battery_indicator() {
    if (!g_battery_text || !g_battery_fill) return;

    const BatteryMonitor::Status st = BatteryMonitor::status();
    char text[16];
    int percent = 0;
    if (!st.available || !st.battery_present) {
        std::snprintf(text, sizeof(text), st.vbus_present ? "USB" : "--%%");
    } else {
        percent = std::clamp<int>(st.percent, 0, 100);
        std::snprintf(text, sizeof(text), st.charging ? "%d%%+" : "%d%%", percent);
    }
    lv_label_set_text(g_battery_text, text);

    // Interior of the battery outline is 24 px wide. Keep a tiny visible
    // sliver at 0 % and scale linearly up to the full inner width.
    const int fill_width = st.available && st.battery_present
        ? std::max(2, (24 * percent) / 100)
        : 2;
    lv_obj_set_width(g_battery_fill, fill_width);

    uint32_t color = kText;
    if (st.available && st.battery_present && percent <= 15) color = kOrp;
    else if (st.charging) color = 0x30D158;
    lv_obj_set_style_bg_color(g_battery_fill, lv_color_hex(color), 0);
}

void battery_ui_timer_cb(lv_timer_t *) {
    update_battery_indicator();
}

void create_battery_indicator() {
    if (!g_screen) return;

    lv_obj_t *wrap = lv_obj_create(g_screen);
    lv_obj_set_size(wrap, 86, 28);
    lv_obj_align(wrap, LV_ALIGN_TOP_RIGHT, -22, 5);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wrap, 0, 0);
    lv_obj_set_style_pad_all(wrap, 0, 0);
    lv_obj_remove_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(wrap, LV_OBJ_FLAG_CLICKABLE);

    g_battery_text = make_label(wrap, "--%", &lv_font_montserrat_14, kMuted);
    lv_obj_set_width(g_battery_text, 48);
    lv_obj_set_style_text_align(g_battery_text, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(g_battery_text, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_remove_flag(g_battery_text, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *body = lv_obj_create(wrap);
    lv_obj_set_size(body, 30, 16);
    lv_obj_align(body, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_border_color(body, lv_color_hex(kMuted), 0);
    lv_obj_set_style_radius(body, 3, 0);
    lv_obj_set_style_pad_all(body, 2, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE);

    g_battery_fill = lv_obj_create(body);
    lv_obj_set_height(g_battery_fill, 10);
    lv_obj_set_width(g_battery_fill, 2);
    lv_obj_align(g_battery_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_border_width(g_battery_fill, 0, 0);
    lv_obj_set_style_radius(g_battery_fill, 1, 0);
    lv_obj_set_style_bg_color(g_battery_fill, lv_color_hex(kText), 0);
    lv_obj_remove_flag(g_battery_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(g_battery_fill, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *cap = lv_obj_create(wrap);
    lv_obj_set_size(cap, 3, 8);
    lv_obj_align(cap, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_border_width(cap, 0, 0);
    lv_obj_set_style_radius(cap, 1, 0);
    lv_obj_set_style_bg_color(cap, lv_color_hex(kMuted), 0);
    lv_obj_remove_flag(cap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(cap, LV_OBJ_FLAG_CLICKABLE);

    update_battery_indicator();
    g_battery_timer = lv_timer_create(battery_ui_timer_cb, 15000, nullptr);
}




void update_clock_indicator() {
    if (!g_clock_text) return;
    const RtcClock::Time t = RtcClock::now();
    char text[8] = "--:--";
    if (t.valid) std::snprintf(text, sizeof(text), "%02d:%02d", t.hour, t.minute);
    lv_label_set_text(g_clock_text, text);
}

void clock_ui_timer_cb(lv_timer_t *) {
    update_clock_indicator();
}

void create_clock_indicator() {
    if (!g_screen) return;
    g_clock_text = make_label(g_screen, "--:--", &lv_font_montserrat_14, kMuted);
    lv_obj_set_width(g_clock_text, 52);
    lv_obj_set_style_text_align(g_clock_text, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(g_clock_text, LV_ALIGN_TOP_RIGHT, -28, 34);
    lv_obj_remove_flag(g_clock_text, LV_OBJ_FLAG_CLICKABLE);
    update_clock_indicator();
    g_clock_timer = lv_timer_create(clock_ui_timer_cb, 60000, nullptr);
}


void power_badge_opa(void *obj, int32_t value) {
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(obj), static_cast<lv_opa_t>(value), 0);
}

void power_badge_done(lv_anim_t *anim) {
    lv_obj_delete(static_cast<lv_obj_t *>(anim->var));
}

void show_power_badge(const char *text) {
    lv_obj_t *badge = make_label(g_screen, text, &lv_font_montserrat_36, kText);
    lv_obj_set_style_bg_color(badge, lv_color_hex(kAccent), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(badge, 18, 0);
    lv_obj_set_style_pad_hor(badge, 22, 0);
    lv_obj_set_style_pad_ver(badge, 10, 0);
    lv_obj_set_style_opa(badge, LV_OPA_TRANSP, 0);
    lv_obj_center(badge);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, badge);
    lv_anim_set_exec_cb(&anim, power_badge_opa);
    lv_anim_set_values(&anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&anim, 180);
    lv_anim_set_reverse_delay(&anim, 180);
    lv_anim_set_reverse_duration(&anim, 220);
    lv_anim_set_completed_cb(&anim, power_badge_done);
    lv_anim_start(&anim);
}

void show_shutdown_ui() {
    clear_screen(false);
    g_display_sleeping = false;
    bsp_display_brightness_set(g_brightness);
    lv_obj_t *label = make_label(g_screen, "OFF", &lv_font_montserrat_36, kText);
    lv_obj_center(label);
    if (g_touch_indev) {
        lv_indev_reset(g_touch_indev, nullptr);
        lv_indev_enable(g_touch_indev, false);
    }
    // Let LVGL flush the brief OFF message before removing system power.
    g_shutdown_deadline_us = esp_timer_get_time() + 650000;
}

void request_shutdown() {
    if (g_shutdown_pending) return;
    if (g_home_after_transfer_stop || g_book_loading) {
        g_shutdown_requested = true;
        return;
    }
    g_shutdown_requested = false;
    g_shutdown_pending = true;
    if (g_reader_mode) stop_reader(true);
    save_settings();
    // Preserve the existing safe Wi-Fi teardown before redrawing the display.
    if (g_transfer_mode == TransferMode::Wifi) {
        g_shutdown_after_transfer_stop = true;
        g_home_after_transfer_stop = false;
        if (g_transfer_timer) {
            lv_timer_delete(g_transfer_timer);
            g_transfer_timer = nullptr;
        }
        stop_active_transfer();
        return;
    }
    show_shutdown_ui();
}

lv_obj_t *make_card(lv_obj_t *parent, const char *title, const char *subtitle,
                        lv_event_cb_t cb, int32_t height, void *user_data = nullptr) {
    lv_obj_t *card = lv_button_create(parent);
    lv_obj_set_width(card, 336);
    lv_obj_set_height(card, height);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(kCard), 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(kCardPressed), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_set_style_pad_left(card, 18, 0);
    lv_obj_set_style_pad_right(card, 18, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *t = make_label(card, title, &rsvp_unicode_18, kText);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, subtitle ? -12 : 0);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);

    if (subtitle) {
        lv_obj_t *sub = make_label(card, subtitle, &rsvp_unicode_14, kMuted);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
        lv_obj_set_width(sub, 270);
        lv_obj_align(sub, LV_ALIGN_LEFT_MID, 0, 15);
        lv_obj_remove_flag(sub, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *arrow = make_label(card, ">", &lv_font_montserrat_18, kMuted);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_remove_flag(arrow, LV_OBJ_FLAG_CLICKABLE);
    return card;
}

lv_obj_t *make_small_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, int32_t width) {
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, width, 48);
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(kCard), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(kCardPressed), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *label = make_label(button, text, &lv_font_montserrat_14, kText);
    lv_obj_center(label);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return button;
}

lv_obj_t *make_direct_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb,
                             int32_t width, int32_t height = 52) {
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_radius(button, 16, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x24242A), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(kAccent), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *label = make_label(button, text, &rsvp_unicode_18, kText);
    lv_obj_center(label);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return button;
}

void show_home();
void show_library();
void show_settings();
void show_language();
void show_wifi_transfer();
void start_book_open(const std::string &path);
void book_open_task(void *arg);
void build_reader_ui();

void show_loading(const char *title, const char *subtitle) {
    clear_screen();
    lv_obj_t *spinner = lv_spinner_create(g_screen);
    lv_obj_set_size(spinner, 58, 58);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -55);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(kCard), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(kAccent), LV_PART_INDICATOR);

    lv_obj_t *t = make_label(g_screen, title ? title : tr("Buch wird geladen", "Loading book"), &lv_font_montserrat_18, kText);
    lv_obj_set_width(t, 320);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 20);

    lv_obj_t *sub = make_label(g_screen, subtitle ? subtitle : tr("Bitte kurz warten ...", "Please wait ..."), &lv_font_montserrat_14, kMuted);
    lv_obj_set_width(sub, 310);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 53);

    // Force one refresh before the CPU starts EPUB parsing.
    lv_refr_now(nullptr);
}

void stop_reader(bool save = true) {
    g_playing = false;
    if (g_reader_timer) lv_timer_pause(g_reader_timer);
    if (g_play_hint) lv_label_set_text(g_play_hint, tr("Tippen zum Starten", "Tap to play"));
    if (save) save_position();
}

void overlay_hide_cb(lv_timer_t *timer) {
    if (g_overlay) lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(timer);
}

void show_overlay(const std::string &text) {
    if (!g_overlay) return;
    lv_label_set_text(g_overlay, text.c_str());
    lv_obj_remove_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_overlay);
    if (g_overlay_timer) {
        lv_timer_set_period(g_overlay_timer, 650);
        lv_timer_reset(g_overlay_timer);
        lv_timer_resume(g_overlay_timer);
    }
}

void update_reader_meta() {
    // The speed label also exists on the Settings screen where no book is
    // necessarily open. Always refresh it before checking reader validity.
    if (g_speed_label) {
        char speed[24];
        std::snprintf(speed, sizeof(speed), "%u WPM", g_wpm);
        lv_label_set_text(g_speed_label, speed);
    }
    if (!g_reader.valid()) return;
    if (g_progress_label) {
        char progress[48];
        const int pct = static_cast<int>(g_reader.progress() * 100.0f);
        const size_t chapters = g_reader.chapter_count();
        const size_t chapter = g_reader.chapter_index();
        if (chapters > 1 && chapter > 0) {
            std::snprintf(progress, sizeof(progress), tr("%d%%  Kap. %u/%u", "%d%%  Ch. %u/%u"),
                          pct, static_cast<unsigned>(chapter), static_cast<unsigned>(chapters));
        } else {
            std::snprintf(progress, sizeof(progress), "%d%%", pct);
        }
        lv_label_set_text(g_progress_label, progress);
    }
    if (g_progress_bar) lv_bar_set_value(g_progress_bar, static_cast<int32_t>(g_reader.progress() * 100.0f), LV_ANIM_OFF);
}

void render_word(const std::string &word) {
    if (!g_before || !g_orp || !g_after) return;

    std::string before, orp, after;
    ReaderEngine::split_orp(word, before, orp, after);

    lv_label_set_text(g_before, before.c_str());
    lv_label_set_text(g_orp, orp.c_str());
    lv_label_set_text(g_after, after.c_str());

    // Choose by the REAL rendered pixel width of the complete word.
    // The ORP remains centered whenever possible. Only if a word would hit an
    // edge do we move the ORP just far enough to keep the larger font visible.
    // This avoids shrinking words merely because one side of the ORP is long.
    const lv_font_t *fonts[] = {
        &rsvp_unicode_36,
        &rsvp_unicode_28,
        &rsvp_unicode_24,
        &rsvp_unicode_18,
        &rsvp_unicode_14,
    };

    constexpr int kWordGap = 2;
    constexpr int kEdgeMargin = 4;
    // A first render may run before LVGL has finished laying out the screen.
    // Never interpret a zero-width parent as a reason to select the tiny font.
    lv_obj_t *reader_area = lv_obj_get_parent(g_orp);
    lv_obj_update_layout(reader_area);
    int area_width = lv_obj_get_content_width(reader_area);
    if (area_width <= 2 * kEdgeMargin) {
        area_width = lv_obj_get_width(reader_area);
    }
    if (area_width <= 2 * kEdgeMargin) {
        ESP_LOGW(TAG, "Reader width not ready; postponing word layout");
        return;
    }
    const int usable_width = area_width - 2 * kEdgeMargin;

    const lv_font_t *selected_font = fonts[sizeof(fonts) / sizeof(fonts[0]) - 1];
    int selected_before_width = 0;
    int selected_focus_width = 0;
    int selected_after_width = 0;

    for (const lv_font_t *font : fonts) {
        lv_obj_set_style_text_font(g_before, font, 0);
        lv_obj_set_style_text_font(g_orp, font, 0);
        lv_obj_set_style_text_font(g_after, font, 0);

        // Force LVGL to calculate the actual glyph widths for this font.
        lv_obj_update_layout(g_before);
        lv_obj_update_layout(g_orp);
        lv_obj_update_layout(g_after);

        const int before_width = lv_obj_get_width(g_before);
        const int focus_width = lv_obj_get_width(g_orp);
        const int after_width = lv_obj_get_width(g_after);
        const int left_gap = before.empty() ? 0 : kWordGap;
        const int right_gap = after.empty() ? 0 : kWordGap;
        const int total_width = before_width + left_gap + focus_width +
                                right_gap + after_width;

        selected_font = font;
        selected_before_width = before_width;
        selected_focus_width = focus_width;
        selected_after_width = after_width;

        // Fonts are ordered largest -> smallest: first complete-word fit wins.
        if (total_width <= usable_width) break;
    }

    // Re-apply the selected font for a deterministic final layout.
    lv_obj_set_style_text_font(g_before, selected_font, 0);
    lv_obj_set_style_text_font(g_orp, selected_font, 0);
    lv_obj_set_style_text_font(g_after, selected_font, 0);
    lv_obj_update_layout(g_before);
    lv_obj_update_layout(g_orp);
    lv_obj_update_layout(g_after);

    selected_before_width = lv_obj_get_width(g_before);
    selected_focus_width = lv_obj_get_width(g_orp);
    selected_after_width = lv_obj_get_width(g_after);

    const int left_gap = before.empty() ? 0 : kWordGap;
    const int right_gap = after.empty() ? 0 : kWordGap;
    const int left_extent = selected_before_width + left_gap;
    const int right_extent = right_gap + selected_after_width;

    // Valid ORP-center range that keeps the whole word inside the reader area.
    const int nominal_center = area_width / 2;
    const int min_center = kEdgeMargin + left_extent + selected_focus_width / 2;
    const int max_center = area_width - kEdgeMargin - right_extent -
                           (selected_focus_width - selected_focus_width / 2);

    int focus_center = nominal_center;
    if (min_center <= max_center) {
        focus_center = std::clamp(nominal_center, min_center, max_center);
    }
    const int focus_offset = focus_center - nominal_center;

    // Center ORP for normal words; shift only as much as necessary for a long,
    // asymmetric word. This preserves RSVP focus while using much more width.
    lv_obj_align(g_orp, LV_ALIGN_CENTER, focus_offset, -6);
    lv_obj_align_to(g_before, g_orp, LV_ALIGN_OUT_LEFT_MID,
                    before.empty() ? 0 : -kWordGap, 0);
    lv_obj_align_to(g_after, g_orp, LV_ALIGN_OUT_RIGHT_MID,
                    after.empty() ? 0 : kWordGap, 0);

    update_reader_meta();
}

void reader_tick_cb(lv_timer_t *timer) {
    std::string word;
    if (!g_reader.next(word)) {
        stop_reader();
        if (g_play_hint) lv_label_set_text(g_play_hint, tr("Ende des Buches", "End of book"));
        show_overlay(tr("Ende", "End"));
        return;
    }
    render_word(word);
    lv_timer_set_period(timer, ReaderEngine::delay_ms(word, g_wpm, g_sentence_pause_pct, g_clause_pause_pct));
    if (++g_words_since_save >= 25) save_position();
}

void toggle_play() {
    if (!g_reader_timer || !g_reader.valid()) return;
    if (g_playing) {
        stop_reader();
        show_overlay(tr("Pause", "Pause"));
        return;
    }
    std::string word;
    if (!g_reader.current(word)) return;
    g_playing = true;
    if (g_play_hint) lv_label_set_text(g_play_hint, tr("Tippen zum Pausieren", "Tap to pause"));
    lv_timer_set_period(g_reader_timer, ReaderEngine::delay_ms(word, g_wpm, g_sentence_pause_pct, g_clause_pause_pct));
    lv_timer_reset(g_reader_timer);
    lv_timer_resume(g_reader_timer);
    show_overlay(tr("Start", "Play"));
}

void change_wpm(int delta) {
    const int next = std::clamp<int>(static_cast<int>(g_wpm) + delta, kMinWpm, kMaxWpm);
    g_wpm = static_cast<uint16_t>(next);
    save_settings();
    update_reader_meta();
    char text[32];
    std::snprintf(text, sizeof(text), "%u WPM", g_wpm);
    show_overlay(text);
    if (g_playing && g_reader_timer) {
        std::string word;
        if (g_reader.current(word)) {
            lv_timer_set_period(g_reader_timer, ReaderEngine::delay_ms(word, g_wpm, g_sentence_pause_pct, g_clause_pause_pct));
            lv_timer_reset(g_reader_timer);
        }
    }
}

void step_word(bool forward) {
    stop_reader();
    std::string word;
    const bool ok = forward ? g_reader.next(word) : g_reader.previous(word);
    if (ok) render_word(word);
    save_position();
    show_overlay(forward ? tr("Naechstes Wort  >", "Next word  >") : tr("<  Vorheriges Wort", "<  Previous word"));
}



bool reader_touch_zone(const lv_point_t &p) {
    // Exclude the top navigation/header area and the extreme bottom edge.
    return p.y >= 82 && p.y <= 410 && p.x >= 4 && p.x <= 364;
}

void reader_touch_timer_cb(lv_timer_t *) {
    if (!g_reader_mode || !g_touch_indev || !g_reader.valid()) return;

    lv_point_t p{};
    lv_indev_get_point(g_touch_indev, &p);
    const lv_indev_state_t state = lv_indev_get_state(g_touch_indev);

    if (state == LV_INDEV_STATE_PRESSED) {
        touch_activity();
        if (!g_reader_touch_active) {
            g_reader_touch_start = p;
            g_reader_touch_last = p;
            g_reader_touch_active = reader_touch_zone(p);
            g_reader_gesture_fired = false;

            if (g_reader_touch_active) {
                ESP_LOGI(TAG, "Reader touch DOWN x=%d y=%d", p.x, p.y);
            }
            return;
        }


        if (!g_reader_touch_active || g_reader_gesture_fired) return;

        g_reader_touch_last = p;

        const int dx = static_cast<int>(p.x) -
                       static_cast<int>(g_reader_touch_start.x);
        const int dy = static_cast<int>(p.y) -
                       static_cast<int>(g_reader_touch_start.y);
        const int ax = std::abs(dx);
        const int ay = std::abs(dy);

        // Directly evaluate the live pointer state. This does not depend on
        // LVGL gesture events or input-device event callbacks.
        constexpr int kSwipeThreshold = 16;
        constexpr int kAxisMargin = 3;

        if (ay >= kSwipeThreshold && ay >= ax + kAxisMargin) {
            g_reader_gesture_fired = true;
            stop_reader();

            if (dy < 0) {
                ESP_LOGI(TAG, "Reader POLL swipe UP dx=%d dy=%d -> +%u WPM",
                         dx, dy, kWpmStep);
                change_wpm(static_cast<int>(kWpmStep));
            } else {
                ESP_LOGI(TAG, "Reader POLL swipe DOWN dx=%d dy=%d -> -%u WPM",
                         dx, dy, kWpmStep);
                change_wpm(-static_cast<int>(kWpmStep));
            }
            return;
        }

        if (ax >= kSwipeThreshold && ax >= ay + kAxisMargin) {
            g_reader_gesture_fired = true;

            if (dx < 0) {
                ESP_LOGI(TAG, "Reader POLL swipe LEFT dx=%d dy=%d -> next",
                         dx, dy);
                step_word(true);
            } else {
                ESP_LOGI(TAG, "Reader POLL swipe RIGHT dx=%d dy=%d -> previous",
                         dx, dy);
                step_word(false);
            }
            return;
        }

        return;
    }

    // RELEASED
    if (!g_reader_touch_active) return;

    const int dx = static_cast<int>(g_reader_touch_last.x) -
                   static_cast<int>(g_reader_touch_start.x);
    const int dy = static_cast<int>(g_reader_touch_last.y) -
                   static_cast<int>(g_reader_touch_start.y);
    const int ax = std::abs(dx);
    const int ay = std::abs(dy);

    const bool fired = g_reader_gesture_fired;
    g_reader_touch_active = false;
    g_reader_gesture_fired = false;

    if (!fired && ax <= 12 && ay <= 12) {
        ESP_LOGI(TAG, "Reader POLL tap x=%d y=%d",
                 g_reader_touch_last.x, g_reader_touch_last.y);
        toggle_play();
    }
}

void home_cb(lv_event_t *) {
    stop_reader();
    g_reader.close();

    // Leaving WLAN is special: do not redraw a full screen while the Wi-Fi
    // driver still owns scarce internal/DMA memory needed by the AMOLED SPI
    // path. Keep the current screen static, tear Wi-Fi down in the worker,
    // then render Home after the memory has actually been returned.
    if (g_transfer_mode == TransferMode::Wifi || g_home_after_transfer_stop) {
        if (g_home_after_transfer_stop) return;
        g_home_after_transfer_stop = true;
        if (g_transfer_timer) {
            lv_timer_delete(g_transfer_timer);
            g_transfer_timer = nullptr;
        }
        if (g_transfer_status) {
            lv_label_set_text(g_transfer_status,
                tr("WLAN wird beendet ...", "Stopping Wi-Fi ..."));
        }
        stop_active_transfer();
        return;
    }

    show_home();
}

void library_cb(lv_event_t *) { show_library(); }
void settings_cb(lv_event_t *) { show_settings(); }
void language_cb(lv_event_t *) { show_language(); }
void wifi_transfer_cb(lv_event_t *) { show_wifi_transfer(); }

void continue_cb(lv_event_t *) {
    if (file_exists(g_last_book)) start_book_open(g_last_book);
    else show_library();
}

void speed_down_cb(lv_event_t *) {
    ESP_LOGI(TAG, "Settings WPM down: %u -> %d", g_wpm, std::max<int>(kMinWpm, static_cast<int>(g_wpm) - kWpmStep));
    change_wpm(-static_cast<int>(kWpmStep));
}
void speed_up_cb(lv_event_t *) {
    ESP_LOGI(TAG, "Settings WPM up: %u -> %d", g_wpm, std::min<int>(kMaxWpm, static_cast<int>(g_wpm) + kWpmStep));
    change_wpm(kWpmStep);
}

void book_cb(lv_event_t *event) {
    auto *path = static_cast<std::string *>(lv_event_get_user_data(event));
    if (path) start_book_open(*path);
}

void set_language_cb(lv_event_t *event) {
    const intptr_t value = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
    g_language = value == 1 ? Language::English : Language::German;
    save_settings();
    show_home();
}


void recent_book_cb(lv_event_t *event) {
    auto *path = static_cast<std::string *>(lv_event_get_user_data(event));
    if (path && file_exists(*path)) start_book_open(*path);
}



void brightness_down_cb(lv_event_t *) {
    g_brightness = static_cast<uint8_t>(std::max<int>(10, g_brightness - 10));
    bsp_display_brightness_set(g_brightness);
    save_settings();
    if (g_brightness_label) {
        char text[24]; std::snprintf(text, sizeof(text), "%u%%", g_brightness);
        lv_label_set_text(g_brightness_label, text);
    }
    touch_activity();
}

void brightness_up_cb(lv_event_t *) {
    g_brightness = static_cast<uint8_t>(std::min<int>(100, g_brightness + 10));
    bsp_display_brightness_set(g_brightness);
    save_settings();
    if (g_brightness_label) {
        char text[24]; std::snprintf(text, sizeof(text), "%u%%", g_brightness);
        lv_label_set_text(g_brightness_label, text);
    }
    touch_activity();
}

const uint8_t kSleepChoices[] = {0, 1, 2, 5, 10, 20};
size_t sleep_choice_index() {
    for (size_t i = 0; i < sizeof(kSleepChoices); ++i) if (kSleepChoices[i] == g_auto_sleep_minutes) return i;
    return 0;
}
void update_sleep_label() {
    if (!g_sleep_label) return;
    char text[32];
    if (g_auto_sleep_minutes == 0) std::snprintf(text, sizeof(text), "%s", tr("Aus", "Off"));
    else std::snprintf(text, sizeof(text), tr("%u Min.", "%u min."), g_auto_sleep_minutes);
    lv_label_set_text(g_sleep_label, text);
}
void sleep_down_cb(lv_event_t *) {
    size_t i = sleep_choice_index();
    if (i > 0) --i;
    g_auto_sleep_minutes = kSleepChoices[i];
    save_settings(); update_sleep_label(); touch_activity();
}
void sleep_up_cb(lv_event_t *) {
    size_t i = sleep_choice_index();
    const size_t n = sizeof(kSleepChoices) / sizeof(kSleepChoices[0]);
    if (i + 1 < n) ++i;
    g_auto_sleep_minutes = kSleepChoices[i];
    save_settings(); update_sleep_label(); touch_activity();
}

void update_pause_labels() {
    if (g_sentence_pause_label) {
        char t[24]; std::snprintf(t, sizeof(t), "+%u%%", g_sentence_pause_pct);
        lv_label_set_text(g_sentence_pause_label, t);
    }
    if (g_clause_pause_label) {
        char t[24]; std::snprintf(t, sizeof(t), "+%u%%", g_clause_pause_pct);
        lv_label_set_text(g_clause_pause_label, t);
    }
}
void sentence_pause_down_cb(lv_event_t *) {
    g_sentence_pause_pct = static_cast<uint16_t>(std::max<int>(0, g_sentence_pause_pct - 25));
    save_settings(); update_pause_labels();
}
void sentence_pause_up_cb(lv_event_t *) {
    g_sentence_pause_pct = static_cast<uint16_t>(std::min<int>(200, g_sentence_pause_pct + 25));
    save_settings(); update_pause_labels();
}
void clause_pause_down_cb(lv_event_t *) {
    g_clause_pause_pct = static_cast<uint16_t>(std::max<int>(0, g_clause_pause_pct - 10));
    save_settings(); update_pause_labels();
}
void clause_pause_up_cb(lv_event_t *) {
    g_clause_pause_pct = static_cast<uint16_t>(std::min<int>(150, g_clause_pause_pct + 10));
    save_settings(); update_pause_labels();
}

void delete_book_request_cb(lv_event_t *event) {
    auto *path = static_cast<std::string *>(lv_event_get_user_data(event));
    if (path) show_delete_confirm(*path);
}

void delete_cancel_cb(lv_event_t *) {
    g_pending_delete.clear();
    show_library();
}

void delete_confirm_cb(lv_event_t *) {
    const std::string path = g_pending_delete;
    if (path.empty()) { show_library(); return; }

    if (g_reader.valid() && g_active_book_key == path) {
        stop_reader(false);
        g_reader.close();
    }

    remove_file_if_exists(path);

    if (has_extension(path, ".epub")) {
        char cache[96];
        std::snprintf(cache, sizeof(cache), "%s/.rsvp6_%08lx.txt", BSP_SD_MOUNT_POINT,
                      static_cast<unsigned long>(fnv1a(path.c_str())));
        remove_file_if_exists(cache);
        remove_file_if_exists(std::string(cache) + ".chap");
        // Clean old cache versions as well.
        std::snprintf(cache, sizeof(cache), "%s/.rsvp6_%08lx.txt", BSP_SD_MOUNT_POINT,
                      static_cast<unsigned long>(fnv1a(path.c_str())));
        remove_file_if_exists(cache);
        remove_file_if_exists(std::string(cache) + ".chap");
    }

    nvs_handle_t ph;
    if (nvs_open("positions", NVS_READWRITE, &ph) == ESP_OK) {
        nvs_erase_key(ph, position_key(path).c_str());
        nvs_commit(ph);
        nvs_close(ph);
    }

    forget_recent(path);
    save_settings();
    g_pending_delete.clear();
    scan_books();
    show_library();
}

void power_timer_cb(lv_timer_t *) {
    // This callback runs in LVGL context; the worker only signals completion.
    if (g_transfer_stop_complete.exchange(false, std::memory_order_acq_rel)) {
        if (g_shutdown_after_transfer_stop) {
            g_shutdown_after_transfer_stop = false;
            g_home_after_transfer_stop = false;
            ESP_LOGI(TAG, "Transfer stop complete: showing shutdown screen");
            show_shutdown_ui();
        } else if (g_home_after_transfer_stop) {
            g_home_after_transfer_stop = false;
            ESP_LOGI(TAG, "Transfer stop complete: showing home screen");
            show_home();
        }
    }
    if (g_shutdown_pending) {
        if (g_shutdown_deadline_us && esp_timer_get_time() >= g_shutdown_deadline_us) {
            g_shutdown_deadline_us = esp_timer_get_time() + 1000000;
            if (!BatteryMonitor::shutdown()) {
                ESP_LOGE(TAG, "PMU shutdown failed; retrying (hardware PWR fallback: 6s)");
            }
        }
        return;
    }
    if (g_shutdown_requested || BatteryMonitor::consume_power_key_long_press()) {
        request_shutdown();
        return;
    }
    if (!g_touch_indev) return;

    if (lv_indev_get_state(g_touch_indev) == LV_INDEV_STATE_PRESSED) {
        touch_activity();
        return;
    }

    if (g_display_sleeping || g_auto_sleep_minutes == 0 || g_playing ||
        g_book_loading || g_transfer_mode != TransferMode::None ||
        g_home_after_transfer_stop) return;
    if (g_last_activity_us == 0) g_last_activity_us = esp_timer_get_time();

    const int64_t idle_us = esp_timer_get_time() - g_last_activity_us;
    const int64_t limit_us = static_cast<int64_t>(g_auto_sleep_minutes) * 60LL * 1000000LL;
    if (idle_us >= limit_us) {
        request_shutdown();
    }
}

void boot_timer_cb(lv_timer_t *) {
    static bool raw_pressed = false;
    static bool stable_pressed = false;
    static bool armed = false;
    static int64_t changed_us = 0;
    static int64_t pressed_us = 0;
    const int64_t now = esp_timer_get_time();
    const bool pressed = gpio_get_level(GPIO_NUM_0) == 0;
    if (pressed != raw_pressed) {
        raw_pressed = pressed;
        changed_us = now;
    }
    if (now - changed_us < 30000) return;
    if (!pressed && !stable_pressed) armed = true;
    if (pressed == stable_pressed) return;
    stable_pressed = pressed;
    if (pressed) {
        pressed_us = now;
    } else if (armed && now - pressed_us < 2000000 &&
               !g_shutdown_pending && !g_book_loading) {
        touch_activity();
        home_cb(nullptr);
    }
}

void refresh_transfer_status_cb(lv_timer_t *) {
    if (!g_transfer_status) return;
    std::string text;
    int progress = -1;
    if (g_transfer_mode == TransferMode::Wifi) {
        text = WifiTransfer::status();
        progress = WifiTransfer::progress();
    }
    if (!text.empty()) lv_label_set_text(g_transfer_status, display_safe(text).c_str());
    if (g_transfer_bar) {
        if (progress >= 0) {
            lv_obj_remove_flag(g_transfer_bar, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(g_transfer_bar, std::clamp(progress, 0, 100), LV_ANIM_OFF);
        } else {
            lv_obj_add_flag(g_transfer_bar, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void show_home() {
    clear_screen();
    scan_books();

    lv_obj_t *brand = make_label(g_screen, "RSVP", &lv_font_montserrat_28, kText);
    lv_obj_align(brand, LV_ALIGN_TOP_LEFT, 18, 14);
    lv_obj_t *reader = make_label(g_screen, "READER", &lv_font_montserrat_14, kAccent);
    lv_obj_align(reader, LV_ALIGN_TOP_LEFT, 20, 50);

    lv_obj_t *tagline = make_label(g_screen,
        tr("Schneller lesen. Ruhiger fokussieren.", "Read faster. Focus calmly."),
        &lv_font_montserrat_14, kMuted);
    lv_obj_align(tagline, LV_ALIGN_TOP_LEFT, 18, 73);

    lv_obj_t *stack = lv_obj_create(g_screen);
    lv_obj_set_size(stack, 350, 346);
    lv_obj_align(stack, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_opa(stack, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(stack, 0, 0);
    lv_obj_set_style_pad_all(stack, 6, 0);
    lv_obj_set_style_pad_row(stack, 8, 0);
    lv_obj_set_flex_flow(stack, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(stack, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(stack, LV_SCROLLBAR_MODE_AUTO);

    if (file_exists(g_last_book)) {
        const std::string subtitle = display_safe(basename_no_ext(g_last_book));
        make_card(stack, tr("Weiterlesen", "Continue reading"), subtitle.c_str(), continue_cb, 56);
    }

    if (!g_recent_books.empty()) {
        lv_obj_t *recent_title = make_label(stack, tr("Zuletzt gelesen", "Recently read"), &rsvp_unicode_14, kMuted);
        lv_obj_set_width(recent_title, 320);
        for (auto &path : g_recent_books) {
            if (!file_exists(path) || path == g_last_book) continue;
            const std::string title = display_safe(basename_no_ext(path));
            make_card(stack, title.c_str(), nullptr, recent_book_cb, 48, &path);
        }
    }

    char library_sub[96];
    std::snprintf(library_sub, sizeof(library_sub), tr("%u Bücher • %s", "%u books • %s"),
                  static_cast<unsigned>(g_books.size()), format_storage().c_str());
    make_card(stack, tr("Bibliothek", "Library"), library_sub, library_cb, 56);
    make_card(stack, tr("WLAN Upload", "Wi-Fi upload"),
              tr("Hotspot + Browser", "Hotspot + browser"), wifi_transfer_cb, 56);
    make_card(stack, tr("Sprache", "Language"),
              g_language == Language::German ? "Deutsch" : "English", language_cb, 56);

    char setting_sub[96];
    std::snprintf(setting_sub, sizeof(setting_sub), tr("%u WPM • Helligkeit %u%%", "%u WPM • Brightness %u%%"),
                  g_wpm, g_brightness);
    make_card(stack, tr("Einstellungen", "Settings"), setting_sub, settings_cb, 56);
}

void show_library() {
    clear_screen();
    scan_books();

    lv_obj_t *back = make_small_button(g_screen, "<", home_cb, 48);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_obj_t *title = make_label(g_screen, tr("Bibliothek", "Library"), &lv_font_montserrat_24, kText);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 76, 18);

    const std::string storage = format_storage();
    lv_obj_t *sub = make_label(g_screen, storage.c_str(), &rsvp_unicode_14, kMuted);
    lv_obj_set_width(sub, 270);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 76, 51);

    lv_obj_t *list = lv_obj_create(g_screen);
    lv_obj_set_size(list, 348, 356);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_bg_color(list, lv_color_hex(kBg), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    if (!g_sd_mounted) {
        lv_obj_t *msg = make_label(list, tr("microSD nicht erkannt", "microSD not detected"),
                                   &rsvp_unicode_14, kMuted);
        lv_obj_center(msg);
        return;
    }
    if (g_books.empty()) {
        lv_obj_t *msg = make_label(list,
            tr("Keine Bücher gefunden.\nEPUB/TXT per WLAN hochladen.",
               "No books found.\nUpload EPUB/TXT via Wi-Fi."),
            &rsvp_unicode_14, kMuted);
        lv_obj_set_width(msg, 300);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(msg);
        return;
    }

    for (auto &path : g_books) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_size(row, 326, 66);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(kCard), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *open = lv_button_create(row);
        lv_obj_set_size(open, 252, 54);
        lv_obj_align(open, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_bg_opa(open, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(open, lv_color_hex(kCardPressed), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(open, 0, 0);
        lv_obj_add_event_cb(open, book_cb, LV_EVENT_CLICKED, &path);

        const std::string name = display_safe(basename_no_ext(path));
        lv_obj_t *label = make_label(open, name.c_str(), &rsvp_unicode_14, kText);
        lv_obj_set_width(label, 232);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *del = lv_button_create(row);
        lv_obj_set_size(del, 52, 52);
        lv_obj_align(del, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_radius(del, 12, 0);
        lv_obj_set_style_bg_color(del, lv_color_hex(0x2C2020), 0);
        lv_obj_set_style_bg_color(del, lv_color_hex(kOrp), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(del, 0, 0);
        lv_obj_add_event_cb(del, delete_book_request_cb, LV_EVENT_CLICKED, &path);
        lv_obj_t *x = make_label(del, "X", &lv_font_montserrat_18, kText);
        lv_obj_center(x);
        lv_obj_remove_flag(x, LV_OBJ_FLAG_CLICKABLE);
    }
}

void show_settings() {
    clear_screen();

    lv_obj_t *back = make_small_button(g_screen, "<", home_cb, 48);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_obj_t *title = make_label(g_screen, tr("Einstellungen", "Settings"), &lv_font_montserrat_24, kText);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 76, 21);

    lv_obj_t *list = lv_obj_create(g_screen);
    lv_obj_set_size(list, 348, 360);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    auto make_setting = [&](const char *name, lv_obj_t **value_label,
                            const char *value, lv_event_cb_t down, lv_event_cb_t up) {
        lv_obj_t *card = lv_obj_create(list);
        lv_obj_set_size(card, 326, 94);
        lv_obj_set_style_radius(card, 16, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(kCard), 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 10, 0);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *name_label = make_label(card, name, &rsvp_unicode_14, kMuted);
        lv_obj_align(name_label, LV_ALIGN_TOP_LEFT, 2, 0);

        *value_label = make_label(card, value, &rsvp_unicode_18, kText);
        lv_obj_align(*value_label, LV_ALIGN_LEFT_MID, 4, 14);

        lv_obj_t *minus = make_direct_button(card, "-", down, 62, 48);
        lv_obj_align(minus, LV_ALIGN_BOTTOM_RIGHT, -72, 0);
        lv_obj_t *plus = make_direct_button(card, "+", up, 62, 48);
        lv_obj_align(plus, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    };

    char wpm[24];
    std::snprintf(wpm, sizeof(wpm), "%u WPM", g_wpm);
    make_setting(tr("Lesegeschwindigkeit", "Reading speed"), &g_speed_label, wpm,
                 speed_down_cb, speed_up_cb);

    char brightness[24];
    std::snprintf(brightness, sizeof(brightness), "%u%%", g_brightness);
    make_setting(tr("Helligkeit", "Brightness"), &g_brightness_label, brightness,
                 brightness_down_cb, brightness_up_cb);

    char sleep[32];
    if (g_auto_sleep_minutes == 0) std::snprintf(sleep, sizeof(sleep), "%s", tr("Aus", "Off"));
    else std::snprintf(sleep, sizeof(sleep), tr("%u Min.", "%u min."), g_auto_sleep_minutes);
    make_setting(tr("Geraet automatisch aus", "Auto power off"), &g_sleep_label, sleep,
                 sleep_down_cb, sleep_up_cb);

    char sentence[24];
    std::snprintf(sentence, sizeof(sentence), "+%u%%", g_sentence_pause_pct);
    make_setting(tr("Pause am Satzende", "Sentence-end pause"), &g_sentence_pause_label, sentence,
                 sentence_pause_down_cb, sentence_pause_up_cb);

    char clause[24];
    std::snprintf(clause, sizeof(clause), "+%u%%", g_clause_pause_pct);
    make_setting(tr("Pause bei , : ; -", "Pause at , : ; -"), &g_clause_pause_label, clause,
                 clause_pause_down_cb, clause_pause_up_cb);

    lv_obj_t *hint = lv_obj_create(list);
    lv_obj_set_size(hint, 326, 166);
    lv_obj_set_style_radius(hint, 16, 0);
    lv_obj_set_style_bg_color(hint, lv_color_hex(kCard), 0);
    lv_obj_set_style_border_width(hint, 0, 0);
    lv_obj_set_style_pad_all(hint, 14, 0);
    lv_obj_remove_flag(hint, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *txt = make_label(hint,
        tr("Reader\nHoch/Runter   Tempo ±25 WPM\nLinks/Rechts  Wort zurück/vor\nTippen         Start/Pause",
           "Reader\nUp/Down       Speed ±25 WPM\nLeft/Right   Word back/forward\nTap             Play/Pause"),
        &rsvp_unicode_14, kText);
    lv_obj_set_width(txt, 292);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(txt, 8, 0);
    lv_obj_align(txt, LV_ALIGN_TOP_LEFT, 0, 0);
}

void show_language() {
    clear_screen();
    lv_obj_t *back = make_small_button(g_screen, "<", home_cb, 48);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_obj_t *title = make_label(g_screen, tr("Sprache", "Language"), &lv_font_montserrat_24, kText);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 76, 21);
    lv_obj_t *sub = make_label(g_screen,
        tr("Oberflaechensprache waehlen", "Choose interface language"),
        &lv_font_montserrat_14, kMuted);
    lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 18, 72);

    lv_obj_t *stack = lv_obj_create(g_screen);
    lv_obj_set_size(stack, 350, 200);
    lv_obj_align(stack, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_set_style_bg_opa(stack, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(stack, 0, 0);
    lv_obj_set_style_pad_all(stack, 7, 0);
    lv_obj_set_style_pad_row(stack, 12, 0);
    lv_obj_set_flex_flow(stack, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(stack, LV_OBJ_FLAG_SCROLLABLE);

    make_card(stack, g_language == Language::German ? "Deutsch   *" : "Deutsch",
              "German", set_language_cb, 78, reinterpret_cast<void *>(0));

    make_card(stack, g_language == Language::English ? "English   *" : "English",
              "Englisch", set_language_cb, 78, reinterpret_cast<void *>(1));
}

void show_delete_confirm(const std::string &path) {
    g_pending_delete = path;
    clear_screen();

    lv_obj_t *title = make_label(g_screen, tr("Buch löschen?", "Delete book?"), &rsvp_unicode_24, kText);
    lv_obj_set_width(title, 320);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 94);

    const std::string name = display_safe(basename_no_ext(path));
    lv_obj_t *book = make_label(g_screen, name.c_str(), &rsvp_unicode_18, kMuted);
    lv_obj_set_width(book, 300);
    lv_label_set_long_mode(book, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(book, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(book, LV_ALIGN_TOP_MID, 0, 145);

    lv_obj_t *note = make_label(g_screen,
        tr("Die Datei wird dauerhaft von der microSD gelöscht.",
           "The file will be permanently deleted from the microSD."),
        &rsvp_unicode_14, kMuted);
    lv_obj_set_width(note, 300);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(note, LV_ALIGN_TOP_MID, 0, 205);

    lv_obj_t *cancel = make_direct_button(g_screen, tr("Abbrechen", "Cancel"), delete_cancel_cb, 145, 54);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 22, -52);
    lv_obj_t *del = make_direct_button(g_screen, tr("Löschen", "Delete"), delete_confirm_cb, 145, 54);
    lv_obj_align(del, LV_ALIGN_BOTTOM_RIGHT, -22, -52);
    lv_obj_set_style_bg_color(del, lv_color_hex(0xB3261E), 0);
}

void show_wifi_transfer() {
    clear_screen();
    g_transfer_mode = TransferMode::Wifi;

    lv_obj_t *back = make_small_button(g_screen, "<", home_cb, 48);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_obj_t *title = make_label(g_screen, tr("WLAN Upload", "Wi-Fi upload"), &lv_font_montserrat_24, kText);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 76, 21);

    lv_obj_t *card = lv_obj_create(g_screen);
    lv_obj_set_size(card, 340, 278);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 82);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(kCard), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *step1 = make_label(card,
        tr("1. Mit diesem WLAN verbinden:", "1. Connect to this Wi-Fi:"),
        &lv_font_montserrat_14, kMuted);
    lv_obj_align(step1, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *ssid = make_label(card, "RSVP-Reader", &rsvp_unicode_24, kText);
    lv_obj_align(ssid, LV_ALIGN_TOP_LEFT, 0, 24);

    lv_obj_t *step2 = make_label(card,
        tr("2. Passwort:", "2. Password:"),
        &rsvp_unicode_14, kMuted);
    lv_obj_align(step2, LV_ALIGN_TOP_LEFT, 0, 66);

    lv_obj_t *pw = make_label(card, "reader1234", &rsvp_unicode_24, kText);
    lv_obj_align(pw, LV_ALIGN_TOP_LEFT, 0, 88);

    lv_obj_t *step3 = make_label(card,
        tr("3. Im Browser öffnen:", "3. Open in browser:"),
        &rsvp_unicode_14, kMuted);
    lv_obj_align(step3, LV_ALIGN_TOP_LEFT, 0, 132);

    lv_obj_t *ip = make_label(card, "192.168.4.1", &rsvp_unicode_24, kText);
    lv_obj_align(ip, LV_ALIGN_TOP_LEFT, 0, 154);

    lv_obj_t *note = make_label(card,
        tr("EPUB/TXT auswählen und hochladen.", "Choose EPUB/TXT and upload."),
        &rsvp_unicode_14, kMuted);
    lv_obj_set_width(note, 300);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_align(note, LV_ALIGN_TOP_LEFT, 0, 202);

    g_transfer_status = make_label(g_screen, tr("WLAN wird gestartet...", "Starting Wi-Fi..."), &lv_font_montserrat_14, kAccent);
    lv_obj_set_width(g_transfer_status, 330);
    lv_obj_set_style_text_align(g_transfer_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_transfer_status, LV_ALIGN_BOTTOM_MID, 0, -48);

    g_transfer_bar = lv_bar_create(g_screen);
    lv_obj_set_size(g_transfer_bar, 300, 6);
    lv_obj_align(g_transfer_bar, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_bar_set_range(g_transfer_bar, 0, 100);
    lv_obj_add_flag(g_transfer_bar, LV_OBJ_FLAG_HIDDEN);

    if (!g_sd_mounted) {
        lv_label_set_text(g_transfer_status, tr("microSD nicht erkannt", "microSD not detected"));
        return;
    }
    if (!WifiTransfer::start(g_language == Language::English)) {
        lv_label_set_text(g_transfer_status, tr("WLAN konnte nicht gestartet werden", "Could not start Wi-Fi"));
        return;
    }
    g_transfer_timer = lv_timer_create(refresh_transfer_status_cb, 400, nullptr);
}

void error_back_cb(lv_event_t *) { show_library(); }

void show_error(const char *title, const std::string &message) {
    clear_screen();
    lv_obj_t *t = make_label(g_screen, title, &lv_font_montserrat_18, kText);
    lv_obj_set_width(t, 320);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_t *m = make_label(g_screen, message.c_str(), &lv_font_montserrat_14, kMuted);
    lv_obj_set_width(m, 310);
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(m, LV_ALIGN_CENTER, 0, -10);
    lv_obj_t *back = make_small_button(g_screen, tr("Zurueck", "Back"), error_back_cb, 120);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -45);
}

void build_reader_ui() {
    clear_screen();

    lv_obj_t *back = make_small_button(g_screen, "<", home_cb, 48);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 10, 10);

    const std::string safe_title = display_safe(g_active_book_title);
    lv_obj_t *title = make_label(g_screen, safe_title.c_str(), &rsvp_unicode_14, kText);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, 190);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);

    g_progress_label = make_label(g_screen, "0%", &rsvp_unicode_14, kMuted);
    lv_obj_set_width(g_progress_label, 210);
    lv_label_set_long_mode(g_progress_label, LV_LABEL_LONG_DOT);
    lv_obj_align(g_progress_label, LV_ALIGN_TOP_LEFT, 78, 48);

    g_progress_bar = lv_bar_create(g_screen);
    lv_obj_set_size(g_progress_bar, 330, 3);
    lv_obj_align(g_progress_bar, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_set_style_bg_color(g_progress_bar, lv_color_hex(0x2C2C2E), LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_progress_bar, lv_color_hex(kAccent), LV_PART_INDICATOR);
    lv_bar_set_range(g_progress_bar, 0, 100);

    lv_obj_t *reader_area = lv_obj_create(g_screen);
    lv_obj_set_size(reader_area, 354, 250);
    lv_obj_align(reader_area, LV_ALIGN_CENTER, 0, -2);
    lv_obj_set_style_bg_opa(reader_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(reader_area, 0, 0);
    lv_obj_set_style_pad_all(reader_area, 0, 0);
    lv_obj_remove_flag(reader_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(reader_area, LV_OBJ_FLAG_CLICKABLE);

    g_before = make_label(reader_area, "", &lv_font_montserrat_28, kText);
    g_orp = make_label(reader_area, "", &lv_font_montserrat_28, kOrp);
    g_after = make_label(reader_area, "", &lv_font_montserrat_28, kText);
    lv_obj_remove_flag(g_before, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(g_orp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(g_after, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *focus_top = lv_obj_create(reader_area);
    lv_obj_set_size(focus_top, 2, 15);
    lv_obj_set_style_bg_color(focus_top, lv_color_hex(kMuted), 0);
    lv_obj_set_style_border_width(focus_top, 0, 0);
    lv_obj_align(focus_top, LV_ALIGN_CENTER, 0, -55);
    lv_obj_remove_flag(focus_top, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *focus_bottom = lv_obj_create(reader_area);
    lv_obj_set_size(focus_bottom, 2, 15);
    lv_obj_set_style_bg_color(focus_bottom, lv_color_hex(kMuted), 0);
    lv_obj_set_style_border_width(focus_bottom, 0, 0);
    lv_obj_align(focus_bottom, LV_ALIGN_CENTER, 0, 42);
    lv_obj_remove_flag(focus_bottom, LV_OBJ_FLAG_CLICKABLE);

    g_play_hint = make_label(reader_area, tr("Tippen zum Starten", "Tap to play"), &lv_font_montserrat_14, kMuted);
    lv_obj_align(g_play_hint, LV_ALIGN_BOTTOM_MID, 0, -3);
    lv_obj_remove_flag(g_play_hint, LV_OBJ_FLAG_CLICKABLE);

    g_speed_label = make_label(g_screen, "", &lv_font_montserrat_14, kMuted);
    lv_obj_align(g_speed_label, LV_ALIGN_BOTTOM_MID, 0, -19);


    g_overlay = make_label(g_screen, "", &lv_font_montserrat_18, kText);
    lv_obj_set_style_bg_color(g_overlay, lv_color_hex(0x27272D), 0);
    lv_obj_set_style_bg_opa(g_overlay, LV_OPA_90, 0);
    lv_obj_set_style_radius(g_overlay, 12, 0);
    lv_obj_set_style_pad_left(g_overlay, 16, 0);
    lv_obj_set_style_pad_right(g_overlay, 16, 0);
    lv_obj_set_style_pad_top(g_overlay, 9, 0);
    lv_obj_set_style_pad_bottom(g_overlay, 9, 0);
    lv_obj_align(g_overlay, LV_ALIGN_CENTER, 0, 85);
    lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);

    // Reader input is captured directly from the BSP LVGL input device.
    // This bypasses object gesture routing, which did not reliably emit
    // LV_EVENT_GESTURE on the CST820-compatible controller.
    g_reader_mode = true;
    g_reader_touch_active = false;
    g_reader_gesture_fired = false;

    if (!g_reader_touch_timer) {
        g_reader_touch_timer = lv_timer_create(reader_touch_timer_cb, 30, nullptr);
    }

    // The reader screen is newly created here. Resolve its geometry before
    // measuring the first word; otherwise the parent may still report 0 px.
    lv_obj_update_layout(g_screen);
    lv_obj_update_layout(reader_area);

    std::string word;
    if (g_reader.current(word)) render_word(word);
    g_reader_timer = lv_timer_create(reader_tick_cb, ReaderEngine::delay_ms(word, g_wpm, g_sentence_pause_pct, g_clause_pause_pct), nullptr);
    lv_timer_pause(g_reader_timer);
    g_overlay_timer = lv_timer_create(overlay_hide_cb, 650, nullptr);
    lv_timer_pause(g_overlay_timer);
}

void show_worker_error(const char *title, const std::string &message) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (bsp_display_lock(250)) {
            g_book_loading = false;
            show_error(title, message);
            bsp_display_unlock();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    ESP_LOGE(TAG, "Could not acquire LVGL lock to show error");
    g_book_loading = false;
}

void book_open_task(void *arg) {
    std::string path = *static_cast<std::string *>(arg);
    delete static_cast<std::string *>(arg);

    g_active_book_key = path;
    g_active_book_title = basename_no_ext(path);
    g_last_book = path;
    remember_recent(path);
    save_settings();

    ESP_LOGI(TAG, "Book worker started on core %d: %s", xPortGetCoreID(), path.c_str());

    std::string open_path = path;
    if (has_extension(path, ".epub")) {
        ESP_LOGI(TAG, "Opening EPUB: %s", path.c_str());
        ESP_LOGI(TAG, "Heap before EPUB: internal=%u, psram=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

        char cache_path[96];
        std::snprintf(cache_path, sizeof(cache_path), "%s/.rsvp6_%08lx.txt", BSP_SD_MOUNT_POINT,
                      static_cast<unsigned long>(fnv1a(path.c_str())));
        std::string error;
        if (!EpubReader::to_text_cache(path.c_str(), cache_path, error)) {
            ESP_LOGE(TAG, "EPUB conversion failed: %s", error.c_str());
            show_worker_error(tr("EPUB konnte nicht geoeffnet werden", "Could not open EPUB"), error);
            vTaskDelete(nullptr);
            return;
        }
        open_path = cache_path;
        ESP_LOGI(TAG, "EPUB cache ready: %s", open_path.c_str());
        ESP_LOGI(TAG, "Heap after EPUB: internal=%u, psram=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    }

    const size_t position = load_position(path);
    if (!g_reader.open(open_path.c_str(), position)) {
        ESP_LOGE(TAG, "ReaderEngine open failed: %s", open_path.c_str());
        show_worker_error(tr("Buch konnte nicht geoeffnet werden", "Could not open book"),
                          tr("Die Datei ist leer, zu gross oder konnte nicht gelesen werden.",
                             "The file is empty, too large, or could not be read."));
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "Book loaded: %u bytes", static_cast<unsigned>(g_reader.length()));

    bool shown = false;
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (bsp_display_lock(250)) {
            build_reader_ui();
            g_book_loading = false;
            bsp_display_unlock();
            shown = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    if (!shown) {
        ESP_LOGE(TAG, "Could not acquire LVGL lock to show reader");
        g_book_loading = false;
    } else {
        ESP_LOGI(TAG, "Reader UI shown");
    }

    vTaskDelete(nullptr);
}

void start_book_open(const std::string &path) {
    if (g_book_loading || path.empty()) return;
    g_book_loading = true;

    const std::string title = basename_no_ext(path);
    show_loading(has_extension(path, ".epub") ? tr("EPUB wird vorbereitet", "Preparing EPUB") : tr("Buch wird geladen", "Loading book"), title.c_str());

    auto *request = new (std::nothrow) std::string(path);
    if (!request) {
        g_book_loading = false;
        show_error(tr("Nicht genug Speicher", "Not enough memory"), tr("Das Buch konnte nicht zum Laden vorbereitet werden.", "The book could not be prepared for loading."));
        return;
    }

    const BaseType_t result = xTaskCreatePinnedToCore(
        book_open_task,
        "book_open",
        16384,
        request,
        3,
        nullptr,
        1);

    if (result != pdPASS) {
        delete request;
        g_book_loading = false;
        ESP_LOGE(TAG, "Could not create book_open task");
        show_error(tr("Buch konnte nicht geoeffnet werden", "Could not open book"), tr("Hintergrund-Task konnte nicht gestartet werden.", "Background task could not be started."));
    }
}

}  // namespace

extern "C" void app_main(void) {
    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_result);
    configure_power_saving();
    load_settings();
    transfer_common_init();

    ESP_LOGI(TAG, "RSVP Reader V2 RC16 PWR starting");
    ESP_LOGI(TAG, "Target: Waveshare ESP32-S3 Touch AMOLED 1.8 V2 / CO5300 / CST820");

    lv_display_t *display = bsp_display_start();
    if (!display) {
        ESP_LOGE(TAG, "bsp_display_start failed");
        return;
    }
    g_touch_indev = bsp_display_get_input_dev();
    if (g_touch_indev) {
        ESP_LOGI(TAG, "Reader touch polling initialized");
    } else {
        ESP_LOGE(TAG, "BSP touch input device unavailable");
    }
    ESP_ERROR_CHECK(bsp_display_brightness_set(g_brightness));
    g_last_activity_us = esp_timer_get_time();

    // AXP2101 telemetry + POWERON-key timing/IRQ configuration. Failure is non-fatal;
    // the UI will show --% and PWR shutdown control will be unavailable.
    if (!BatteryMonitor::start()) {
        ESP_LOGW(TAG, "Battery/PWR monitor unavailable");
    } else if (!BatteryMonitor::power_key_ready()) {
        ESP_LOGW(TAG, "PWR long-press control unavailable; battery telemetry remains active");
    }
    if (!RtcClock::start()) {
        ESP_LOGW(TAG, "RTC unavailable; clock will show --:--");
    }

    const esp_err_t sd_result = bsp_sdcard_mount();
    g_sd_mounted = sd_result == ESP_OK;
    if (g_sd_mounted) ESP_LOGI(TAG, "microSD mounted: %s", BSP_SD_MOUNT_POINT);
    else ESP_LOGW(TAG, "microSD mount failed: %s", esp_err_to_name(sd_result));

    if (!bsp_display_lock(0)) {
        ESP_LOGE(TAG, "Could not acquire LVGL lock");
        return;
    }
    lv_timer_t *power_timer = lv_timer_create(power_timer_cb, 250, nullptr);
    (void)power_timer;
    gpio_config_t boot_cfg{};
    boot_cfg.pin_bit_mask = 1ULL << GPIO_NUM_0;
    boot_cfg.mode = GPIO_MODE_INPUT;
    boot_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&boot_cfg));
    lv_timer_create(boot_timer_cb, 50, nullptr);
    show_home();
    show_power_badge("ON");
    bsp_display_unlock();
    ESP_LOGI(TAG, "UI ready");
}
