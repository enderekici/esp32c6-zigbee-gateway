#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_timer.h"

#include "board.h"
#include "lcd.h"
#include "zigbee_task.h"
#include "wifi_task.h"
#include "ui_lvgl.h"
#include "led_strip.h"

// Onboard WS2812 RGB LED (GP8). We don't use it; clear it at boot so the
// board isn't glowing green on the back.
#define BOARD_RGB_LED_GPIO 8

static void rgb_led_off(void)
{
    led_strip_config_t scfg = {
        .strip_gpio_num   = BOARD_RGB_LED_GPIO,
        .max_leds         = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model        = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src       = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    led_strip_handle_t strip;
    if (led_strip_new_rmt_device(&scfg, &rmt, &strip) == ESP_OK) {
        led_strip_clear(strip);  // all pixels -> 0,0,0
    }
}

// LVGL dashboard is the primary UI. Set to 0 to fall back to the hand-rolled
// Spleen-font blitter UI below (kept for reference / no-LVGL builds).
#define USE_LVGL_UI 1

static const char *TAG = "app";

// Spleen 8x16 font, 8 px wide x 16 px tall, no extra spacing.
// 172 / 8 = 21 chars per row. Line stride = 17 => 18 rows.
#define CHAR_W   8
#define LINE_H   17

// The IPS panel has rounded corners that clip the outermost ~8 px. Keep all
// text inside this safe rect.
#define SAFE_X   10
#define SAFE_TOP 12
#define SAFE_BOT 12
#define USABLE_W (BOARD_LCD_H_RES - 2 * SAFE_X)
#define USABLE_H (BOARD_LCD_V_RES - SAFE_TOP - SAFE_BOT)

static const uint16_t BLACK = 0x0000;
#if !USE_LVGL_UI
static const uint16_t WHITE = 0xFFFF;

static void short_ieee(char *out, size_t n, const uint8_t a[8])
{
    snprintf(out, n, "%02x%02x%02x%02x", a[3], a[2], a[1], a[0]);
}

// Number of glyph cells that fit inside the safe horizontal band.
#define ROW_CHARS (USABLE_W / CHAR_W)

// Repaint one full-width row in place. Left-justify and pad with spaces so a
// shorter string erases whatever longer text occupied the row last frame —
// no full-screen clear needed, which is what was causing the per-frame flash.
static void draw_row(lcd_t *lcd, int y, const char *s, uint16_t fg)
{
    char b[ROW_CHARS + 1];
    snprintf(b, sizeof(b), "%-*.*s", ROW_CHARS, ROW_CHARS, s);
    lcd_draw_text(lcd, SAFE_X, y, b, fg, BLACK);
}

static void ui_task(void *arg)
{
    lcd_t *lcd = (lcd_t *)arg;
    char line[48];
    zb_ui_status_t st;

    uint16_t cyan   = lcd_rgb(0,   220, 220);
    uint16_t yellow = lcd_rgb(255, 200, 0);
    uint16_t green  = lcd_rgb(0,   220, 60);
    uint16_t red    = lcd_rgb(255, 60,  60);
    uint16_t grey   = lcd_rgb(140, 140, 140);

    uint32_t last_sig = 0xFFFFFFFFu;  // force a clear on the first frame

    while (true) {
        zigbee_task_get_status(&st);
        uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);

        wifi_ui_status_t wst;
        wifi_task_get_status(&wst);

        // Count the device text rows so a join/leave or a first-press (which
        // adds an event line) is reflected in the layout signature below.
        int dev_rows = 0;
        for (int i = 0; i < ZB_MAX_DEVICES; i++) {
            if (!st.devices[i].active) continue;
            dev_rows += st.devices[i].last_event[0] ? 2 : 1;
        }

        // Only the lines' *contents* change every frame (counters, RSSI); the
        // vertical layout changes rarely. Full-clear (which flashes) only when
        // the layout shape shifts; otherwise repaint each row in place.
        uint32_t sig = ((uint32_t)st.state << 24) |
                       ((uint32_t)(st.permit_open ? 1 : 0) << 20) |
                       ((uint32_t)(wst.state == WIFI_UI_CONNECTED ? 1 : 0) << 16) |
                       ((uint32_t)(dev_rows & 0xff) << 8) |
                       ((uint32_t)(st.num_devices & 0xff));
        if (sig != last_sig) {
            lcd_fill(lcd, BLACK);
            last_sig = sig;
        }

        int y = SAFE_TOP;

        draw_row(lcd, y, "C6 ZIGBEE GW", cyan); y += LINE_H;
        draw_row(lcd, y, "------------------", grey); y += LINE_H;

        const char *state_str;
        uint16_t    state_col;
        switch (st.state) {
        case ZB_UI_BOOTING:  state_str = "BOOT";    state_col = yellow; break;
        case ZB_UI_STARTING: state_str = "INIT";    state_col = yellow; break;
        case ZB_UI_FORMING:  state_str = "FORMING"; state_col = yellow; break;
        case ZB_UI_READY:    state_str = "READY";   state_col = green;  break;
        default:             state_str = "ERROR";   state_col = red;    break;
        }
        snprintf(line, sizeof(line), "ZB:%s", state_str);
        draw_row(lcd, y, line, state_col); y += LINE_H;

        if (st.state == ZB_UI_READY) {
            snprintf(line, sizeof(line), "PAN %04x CH %u", st.pan_id, st.channel);
            draw_row(lcd, y, line, WHITE); y += LINE_H;
            char ieee[12];
            short_ieee(ieee, sizeof(ieee), st.ext_addr);
            snprintf(line, sizeof(line), "EUI %s", ieee);
            draw_row(lcd, y, line, WHITE); y += LINE_H;
        }

        if (st.permit_open) {
            snprintf(line, sizeof(line), "JOIN OPEN %lus",
                     (unsigned long)st.permit_remaining_s);
            draw_row(lcd, y, line, yellow); y += LINE_H;
        } else {
            draw_row(lcd, y, "JOIN closed", grey); y += LINE_H;
        }

        snprintf(line, sizeof(line), "DEVICES %d", st.num_devices);
        draw_row(lcd, y, line, WHITE); y += LINE_H;

        const char *wifi_str; uint16_t wifi_col;
        switch (wst.state) {
        case WIFI_UI_CONNECTED:    wifi_str = "OK";   wifi_col = green;  break;
        case WIFI_UI_CONNECTING:   wifi_str = "CONN"; wifi_col = yellow; break;
        case WIFI_UI_DISCONNECTED: wifi_str = "DOWN"; wifi_col = red;    break;
        default:                   wifi_str = "BOOT"; wifi_col = grey;   break;
        }
        if (wst.state == WIFI_UI_CONNECTED) {
            snprintf(line, sizeof(line), "WIFI %s %ddBm", wst.ip, wst.rssi);
        } else {
            snprintf(line, sizeof(line), "WIFI %s", wifi_str);
        }
        draw_row(lcd, y, line, wifi_col); y += LINE_H;

        draw_row(lcd, y, "------------------", grey); y += LINE_H;

        if (st.num_devices == 0) {
            draw_row(lcd, y, "no devices yet", grey); y += LINE_H;
        }
        for (int i = 0; i < ZB_MAX_DEVICES; i++) {
            if (!st.devices[i].active) continue;
            if (y + LINE_H * 2 > BOARD_LCD_V_RES - SAFE_BOT - LINE_H) break;
            snprintf(line, sizeof(line), "%04x", st.devices[i].short_addr);
            draw_row(lcd, y, line, WHITE); y += LINE_H;
            if (st.devices[i].last_event[0]) {
                uint32_t ago = up - st.devices[i].last_event_s;
                snprintf(line, sizeof(line), " %s  %lus",
                         st.devices[i].last_event, (unsigned long)ago);
                draw_row(lcd, y, line, green);
                y += LINE_H;
            }
        }

        snprintf(line, sizeof(line), "UP %lus", (unsigned long)up);
        draw_row(lcd, BOARD_LCD_V_RES - SAFE_BOT - LINE_H, line, grey);

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
#endif // !USE_LVGL_UI

void app_main(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    ESP_LOGI(TAG, "boot: model %d, cores %d, rev %d.%d",
             info.model, info.cores, info.revision / 100, info.revision % 100);

    rgb_led_off();

    static lcd_t lcd;
    ESP_ERROR_CHECK(lcd_init(&lcd));
    lcd_fill(&lcd, BLACK);

    zigbee_task_start();

#if USE_LVGL_UI
    ui_lvgl_start(&lcd);
#else
    lcd_draw_text(&lcd, SAFE_X, SAFE_TOP, "starting zigbee...", lcd_rgb(255,200,0), BLACK);
    xTaskCreate(ui_task, "ui", 4096, &lcd, 4, NULL);
#endif
}
