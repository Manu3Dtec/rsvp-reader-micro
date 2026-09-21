idf_component_register(
    SRCS
        "main.cpp"
        "reader_engine.cpp"
        "epub_book.cpp"
        "epub_reader.cpp"
        "transfer_common.cpp"
        "transfer_wifi.cpp" "unicode_fonts.c"
        "battery_monitor.cpp"
        "rtc_clock.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        waveshare__esp32_s3_touch_amoled_1_8
        nvs_flash
        esp_rom
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        esp_driver_i2c
        esp_pm

    PRIV_REQUIRES esp_timer)
