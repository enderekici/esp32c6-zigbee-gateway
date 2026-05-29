idf_component_register(
    SRCS "main.c" "lcd.c" "font_spleen.c" "zigbee_task.c" "Vernon_ST7789T.c" "wifi_task.c" "http_task.c" "webhook.c" "ui_lvgl.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd nvs_flash esp_timer esp_wifi esp_netif esp_event esp_coex esp_http_server esp_http_client lvgl esp_lvgl_port led_strip
)
