#pragma once

#include "lcd.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up LVGL on the given panel and build the dashboard. Call once at boot
// after lcd_init(). Spawns the esp_lvgl_port task; the dashboard refreshes
// itself from the Zigbee/Wi-Fi status via an LVGL timer.
void ui_lvgl_start(lcd_t *lcd);

#ifdef __cplusplus
}
#endif
