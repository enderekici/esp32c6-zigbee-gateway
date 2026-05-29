#include "ui_lvgl.h"

#include "board.h"
#include "zigbee_task.h"
#include "wifi_task.h"

#include "esp_timer.h"
#include "esp_log.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

static const char *TAG = "ui";

// Ayu-dark palette, matching the web dashboard in http_task.c.
#define COL_BG     0x0b0e14
#define COL_CARD   0x131722
#define COL_BORDER 0x222838
#define COL_CYAN   0x5ccfe6
#define COL_GREEN  0x7ee787
#define COL_YELLOW 0xffd479
#define COL_RED    0xff8a8a
#define COL_GREY   0x7a88a0
#define COL_TEXT   0xd6deeb

// Pre-create this many device cards; STYRBAR is one device but keep headroom.
#define DEV_CARDS 5

static lv_obj_t *lbl_uptime;
static lv_obj_t *lbl_zb;
static lv_obj_t *lbl_pan;
static lv_obj_t *lbl_join;
static lv_obj_t *lbl_wifi;

static struct {
    lv_obj_t *card;
    lv_obj_t *addr;
    lv_obj_t *evt;
} dev_ui[DEV_CARDS];

static lv_obj_t *lbl_nodev;

static void style_card(lv_obj_t *card)
{
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 2, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, lv_pct(100));
    return l;
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, 8, 0);
    lv_obj_set_style_pad_row(scr, 6, 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *title = mk_label(scr, &lv_font_montserrat_20, COL_CYAN);
    lv_label_set_text(title, "C6 ZIGBEE GW");

    lbl_uptime = mk_label(scr, &lv_font_montserrat_14, COL_GREY);
    lv_label_set_text(lbl_uptime, "booting...");

    // Gateway status card.
    lv_obj_t *gw = lv_obj_create(scr);
    style_card(gw);
    lbl_zb   = mk_label(gw, &lv_font_montserrat_16, COL_TEXT);
    lbl_pan  = mk_label(gw, &lv_font_montserrat_14, COL_GREY);
    lbl_join = mk_label(gw, &lv_font_montserrat_14, COL_GREY);
    lbl_wifi = mk_label(gw, &lv_font_montserrat_14, COL_GREY);

    lv_obj_t *hdr = mk_label(scr, &lv_font_montserrat_16, COL_CYAN);
    lv_label_set_text(hdr, "DEVICES");

    lbl_nodev = mk_label(scr, &lv_font_montserrat_14, COL_GREY);
    lv_label_set_text(lbl_nodev, "no devices paired");

    for (int i = 0; i < DEV_CARDS; i++) {
        lv_obj_t *card = lv_obj_create(scr);
        style_card(card);
        dev_ui[i].card = card;
        dev_ui[i].addr = mk_label(card, &lv_font_montserrat_16, COL_TEXT);
        dev_ui[i].evt  = mk_label(card, &lv_font_montserrat_14, COL_GREEN);
        lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_cb(lv_timer_t *t)
{
    (void)t;
    zb_ui_status_t st;
    zigbee_task_get_status(&st);
    wifi_ui_status_t w;
    wifi_task_get_status(&w);
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000LL);

    lv_label_set_text_fmt(lbl_uptime, "uptime %lus", (unsigned long)up);

    const char *zs; uint32_t zc;
    switch (st.state) {
    case ZB_UI_BOOTING:  zs = "BOOT";    zc = COL_YELLOW; break;
    case ZB_UI_STARTING: zs = "INIT";    zc = COL_YELLOW; break;
    case ZB_UI_FORMING:  zs = "FORMING"; zc = COL_YELLOW; break;
    case ZB_UI_READY:    zs = "READY";   zc = COL_GREEN;  break;
    default:             zs = "ERROR";   zc = COL_RED;    break;
    }
    lv_label_set_text_fmt(lbl_zb, "Zigbee  %s", zs);
    lv_obj_set_style_text_color(lbl_zb, lv_color_hex(zc), 0);

    lv_label_set_text_fmt(lbl_pan, "PAN 0x%04x  ch %u", st.pan_id, st.channel);

    if (st.permit_open) {
        lv_label_set_text_fmt(lbl_join, "join open %lus",
                              (unsigned long)st.permit_remaining_s);
        lv_obj_set_style_text_color(lbl_join, lv_color_hex(COL_YELLOW), 0);
    } else {
        lv_label_set_text(lbl_join, "join closed");
        lv_obj_set_style_text_color(lbl_join, lv_color_hex(COL_GREY), 0);
    }

    if (w.state == WIFI_UI_CONNECTED) {
        lv_label_set_text_fmt(lbl_wifi, "Wi-Fi  %s  %ddBm", w.ip, w.rssi);
        lv_obj_set_style_text_color(lbl_wifi, lv_color_hex(COL_GREEN), 0);
    } else {
        lv_label_set_text(lbl_wifi, "Wi-Fi  down");
        lv_obj_set_style_text_color(lbl_wifi, lv_color_hex(COL_RED), 0);
    }

    int slot = 0;
    for (int i = 0; i < ZB_MAX_DEVICES && slot < DEV_CARDS; i++) {
        if (!st.devices[i].active) continue;
        const zb_device_t *d = &st.devices[i];
        lv_obj_clear_flag(dev_ui[slot].card, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(dev_ui[slot].addr, "0x%04x", d->short_addr);
        if (d->last_event[0]) {
            lv_label_set_text_fmt(dev_ui[slot].evt, "%s  %lus ago",
                                  d->last_event,
                                  (unsigned long)(up - d->last_event_s));
            lv_obj_clear_flag(dev_ui[slot].evt, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(dev_ui[slot].evt, LV_OBJ_FLAG_HIDDEN);
        }
        slot++;
    }
    for (int i = slot; i < DEV_CARDS; i++) {
        lv_obj_add_flag(dev_ui[i].card, LV_OBJ_FLAG_HIDDEN);
    }

    if (slot == 0) lv_obj_clear_flag(lbl_nodev, LV_OBJ_FLAG_HIDDEN);
    else           lv_obj_add_flag(lbl_nodev, LV_OBJ_FLAG_HIDDEN);
}

void ui_lvgl_start(lcd_t *lcd)
{
    lvgl_port_cfg_t pcfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&pcfg));

    lvgl_port_display_cfg_t dcfg = {
        .io_handle    = lcd->io,
        .panel_handle = lcd->panel,
        .buffer_size  = BOARD_LCD_H_RES * 40,
        .double_buffer = true,
        .hres         = BOARD_LCD_H_RES,
        .vres         = BOARD_LCD_V_RES,
        .monochrome   = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        // esp_lvgl_port re-applies mirror from these flags during add_disp,
        // overwriting lcd_init's mirror(true,false). Match it here so the
        // panel keeps the working X-mirror (else the whole UI renders flipped).
        .rotation     = { .swap_xy = false, .mirror_x = true, .mirror_y = false },
        .flags = {
            .buff_dma    = true,
            .swap_bytes  = false,  // LVGL9 path already emits the panel's byte order
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&dcfg);
    if (!disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return;
    }

    lvgl_port_lock(0);
    build_ui();
    lv_timer_create(refresh_cb, 1000, NULL);
    refresh_cb(NULL);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LVGL dashboard up");
}
