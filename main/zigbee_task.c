#include "zigbee_task.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_coexist.h"
#include "esp_pm.h"

#include "wifi_task.h"
#include "webhook.h"
#include "esp_zigbee_core.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "zboss_api.h"
#include "zcl/zb_zcl_common.h"

static const char *TAG = "zb";

#define COORD_ENDPOINT       1
#define PERMIT_JOIN_SECS     180
#define MFR_NAME             "\x04""Evam"
#define MODEL_NAME           "\x0a""C6-Gateway"

static SemaphoreHandle_t s_mtx;
static zb_ui_status_t   s_status;

// Last short address seen on any raw ZCL frame. The On/Off (BRIGHT/DIM) events
// arrive via SET_ATTR, whose message carries no source address — but the raw
// frame passes through zb_raw_handler first, so we stash the src here.
static uint16_t s_last_src;

static uint32_t uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000LL);
}

// --- Device-list persistence in NVS ----------------------------------------
// We persist only the array of joined devices. This is independent of Zigbee's
// own zb_storage partition, which already keeps the network/binding state.
// After flash, the network survives but s_status.devices is RAM-only — we'd
// otherwise lose the UI tracking until each device re-announces.
#define NVS_NAMESPACE  "gw"
#define NVS_KEY_DEVS   "devices"

static void devices_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_DEVS, s_status.devices, sizeof(s_status.devices));
    nvs_commit(h);
    nvs_close(h);
}

static void devices_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = sizeof(s_status.devices);
    if (nvs_get_blob(h, NVS_KEY_DEVS, s_status.devices, &sz) == ESP_OK) {
        // Recount active entries and clear transient fields.
        s_status.num_devices = 0;
        for (int i = 0; i < ZB_MAX_DEVICES; i++) {
            if (s_status.devices[i].active) {
                s_status.devices[i].last_event[0] = 0;
                s_status.devices[i].last_event_s = 0;
                s_status.num_devices++;
            }
        }
        ESP_LOGI(TAG, "restored %d device(s) from NVS", s_status.num_devices);
    }
    nvs_close(h);
}

static bool ieee_is_zero(const uint8_t a[8])
{
    for (int i = 0; i < 8; i++) if (a[i]) return false;
    return true;
}

static zb_device_t *device_find_or_alloc_locked(uint16_t short_addr,
                                                const uint8_t *ieee)
{
    // Prefer IEEE match — short address can change between joins for sleepy
    // end-devices, but IEEE is stable. Update short_addr in place.
    if (ieee && !ieee_is_zero(ieee)) {
        for (int i = 0; i < ZB_MAX_DEVICES; i++) {
            if (s_status.devices[i].active &&
                memcmp(s_status.devices[i].ieee_addr, ieee, 8) == 0) {
                s_status.devices[i].short_addr = short_addr;
                return &s_status.devices[i];
            }
        }
    }
    // Fallback: short-addr match (for events where we don't have IEEE).
    for (int i = 0; i < ZB_MAX_DEVICES; i++) {
        if (s_status.devices[i].active &&
            s_status.devices[i].short_addr == short_addr) {
            return &s_status.devices[i];
        }
    }
    // Allocate a new slot.
    for (int i = 0; i < ZB_MAX_DEVICES; i++) {
        if (!s_status.devices[i].active) {
            zb_device_t *d = &s_status.devices[i];
            d->active = true;
            d->short_addr = short_addr;
            if (ieee) memcpy(d->ieee_addr, ieee, 8);
            else      memset(d->ieee_addr, 0, 8);
            d->joined_s = uptime_s();
            d->last_event_s = 0;
            d->last_event[0] = 0;
            s_status.num_devices++;
            return d;
        }
    }
    return NULL;
}

void zigbee_task_get_status(zb_ui_status_t *out)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_mtx);
}

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(mode_mask));
}

void zigbee_task_open_network(uint8_t duration_s)
{
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_bdb_open_network(duration_s);
    esp_zb_lock_release();
}

// --- ZCL action handler — STYRBAR button events land here ---
//
// STYRBAR endpoint 1 emits commands via Find&Bind to whoever exposes matching
// SERVER clusters. We expose On/Off + Level Control + Scenes (server) on EP1
// below, so STYRBAR's: Play/Pause => On/Off Toggle, +/- => Level Control Move,
// arrow buttons => Scenes Recall.
static void record_event_locked_first_active(const char *evt)
{
    for (int i = 0; i < ZB_MAX_DEVICES; i++) {
        if (s_status.devices[i].active) {
            s_status.devices[i].last_event_s = uptime_s();
            snprintf(s_status.devices[i].last_event,
                     sizeof(s_status.devices[i].last_event), "%s", evt);
            return;
        }
    }
}

// Raw ZCL catcher — needed to receive IKEA TradfriArrow on cluster 0x0005
// (manufacturer code 0x117C), which the standard ZCL dispatcher drops.
// Returning false lets normal processing continue for everything else.
static bool zb_raw_handler(uint8_t bufid)
{
    zb_zcl_parsed_hdr_t *hdr = ZB_BUF_GET_PARAM(bufid, zb_zcl_parsed_hdr_t);
    if (!hdr) return false;

    uint16_t cluster = hdr->cluster_id;
    uint8_t  cmd_id  = hdr->cmd_id;
    uint16_t src     = hdr->addr_data.common_data.source.u.short_addr;
    if (src) s_last_src = src;

    // STYRBAR top/bottom long-hold: cluster 0x0008 (Level Control) standard
    // commands. Match the gesture, NOT the resulting attribute writes — those
    // stop once our local level hits 255 even if the button is still held.
    //   cmd 0x05 MoveWithOnOff {mode, rate}  -> press-and-hold (up if mode=0)
    //   cmd 0x07 StopWithOnOff               -> release
    //   cmd 0x01 Move          {mode, rate}  -> rare (already-on hold)
    //   cmd 0x03 Stop                        -> release
    if (cluster == 0x0008 && !hdr->is_manuf_specific) {
        char evt[24] = {0};
        switch (cmd_id) {
        case 0x01: case 0x05: {
            const uint8_t *p = (const uint8_t *)zb_buf_begin(bufid);
            uint8_t mode = (zb_buf_len(bufid) >= 1 && p) ? p[0] : 0;
            snprintf(evt, sizeof(evt), mode ? "DIM HLD" : "BRIGHT HLD");
            break;
        }
        case 0x03: case 0x07:
            snprintf(evt, sizeof(evt), "BRT REL");
            break;
        }
        if (evt[0]) {
            ESP_LOGI(TAG, ">> %s (src=0x%04x cmd=0x%02x)", evt, src, cmd_id);
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            record_event_locked_first_active(evt);
            xSemaphoreGive(s_mtx);
            webhook_send(src, evt);
        }
        return false;
    }

    // STYRBAR arrows: cluster 0x0005, mfg-specific cmds 0x07/0x08/0x09.
    if (cluster == 0x0005 && hdr->is_manuf_specific) {
        const uint8_t *payload = (const uint8_t *)zb_buf_begin(bufid);
        size_t paylen = zb_buf_len(bufid);
        uint16_t v = (paylen >= 2) ? (uint16_t)(payload[0] | (payload[1] << 8)) : 0;
        // For this physical STYRBAR: payload 0x0100 -> RIGHT, 0x0101 -> LEFT.
        const char *dir = (v == 0x0100) ? "RIGHT" : "LEFT";

        char evt[24] = {0};
        switch (cmd_id) {
        case 0x07: snprintf(evt, sizeof(evt), "ARR %s", dir); break;
        case 0x08: snprintf(evt, sizeof(evt), "ARR %s HLD", dir); break;
        case 0x09: snprintf(evt, sizeof(evt), "ARR REL"); break;
        default:   snprintf(evt, sizeof(evt), "ARR cmd 0x%02x", cmd_id); break;
        }
        ESP_LOGI(TAG, ">> %s (src=0x%04x mfg=0x%04x paylen=%u v=0x%04x)",
                 evt, src, hdr->manuf_specific, (unsigned)paylen, v);

        xSemaphoreTake(s_mtx, portMAX_DELAY);
        record_event_locked_first_active(evt);
        xSemaphoreGive(s_mtx);
        webhook_send(src, evt);
        return false;
    }

    ESP_LOGD(TAG, "raw zcl cluster=0x%04x cmd=0x%02x mfg=%d", cluster, cmd_id,
             hdr->is_manuf_specific);
    return false;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t cb_id,
                                   const void *message)
{
    char evt[24] = {0};

    switch (cb_id) {

    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID: {
        const esp_zb_zcl_set_attr_value_message_t *m = message;
        // Log first time we see this (cluster, attr) pair, suppress repeats.
        static uint16_t seen_cluster[16];
        static uint16_t seen_attr[16];
        static int      n_seen = 0;
        bool already = false;
        for (int i = 0; i < n_seen; i++) {
            if (seen_cluster[i] == m->info.cluster &&
                seen_attr[i]    == m->attribute.id) { already = true; break; }
        }
        if (!already && n_seen < 16) {
            seen_cluster[n_seen] = m->info.cluster;
            seen_attr[n_seen]    = m->attribute.id;
            n_seen++;
            ESP_LOGI(TAG, "SET_ATTR cluster=0x%04x ep=%u attr=0x%04x type=0x%02x",
                     m->info.cluster, m->info.dst_endpoint,
                     m->attribute.id, m->attribute.data.type);
        }
        // Map IKEA-style 4-button remote presses to friendly names.
        // Left bright-sun  -> On/Off = 1  -> "BRIGHT"
        // Right dim-sun    -> On/Off = 0  -> "DIM"
        // Top arrow ^      -> Level Move(up)   -> increasing Level series -> "UP"
        // Bottom arrow v   -> Level Move(down) -> decreasing Level series -> "DOWN"
        if (m->info.cluster == 0x0006 && m->attribute.id == 0x0000 &&
            m->attribute.data.value) {
            uint8_t v = *(uint8_t *)m->attribute.data.value;
            snprintf(evt, sizeof(evt), v ? "BRIGHT" : "DIM");
            ESP_LOGI(TAG, ">> %s", evt);
        }
        // Note: we no longer process cluster 0x0008 (Level) attribute writes
        // here — those fire as a burst during a ramp and would overwrite the
        // "BRIGHT HLD" event from the actual Move command. zb_raw_handler
        // catches the Move/Stop commands directly instead.
        break;
    }

    case ESP_ZB_CORE_CMD_CUSTOM_CLUSTER_REQ_CB_ID: {
        const esp_zb_zcl_custom_cluster_command_message_t *m = message;
        ESP_LOGI(TAG, "custom cluster=0x%04x cmd=0x%02x dir=%u size=%u",
                 m->info.cluster, m->info.command.id,
                 m->info.command.direction, m->data.size);
        // IKEA TradfriArrow commands on Scenes cluster (0x0005):
        //   0x07 single  payload uint16: 0x0100=left, 0x0101=right
        //   0x08 hold    payload uint16: same encoding
        //   0x09 release payload uint16: duration ticks
        if (m->info.cluster == 0x0005) {
            const uint8_t *p = (const uint8_t *)m->data.value;
            uint16_t v = (m->data.size >= 2 && p) ? (uint16_t)(p[0] | (p[1] << 8)) : 0;
            const char *dir = (v == 0x0101) ? "RIGHT" : "LEFT";
            switch (m->info.command.id) {
            case 0x07: snprintf(evt, sizeof(evt), "ARR %s", dir); break;
            case 0x08: snprintf(evt, sizeof(evt), "ARR %s HOLD", dir); break;
            case 0x09: snprintf(evt, sizeof(evt), "ARR REL"); break;
            }
            if (evt[0]) ESP_LOGI(TAG, ">> %s", evt);
        }
        break;
    }

    // (No SCENES_RECALL handler — IKEA TradfriArrow uses mfg-specific commands
    // on the same cluster, but they're caught by zb_raw_handler. The standard
    // RecallScene callback was firing with garbage scene IDs on arrow holds.)
    default:
        // First occurrence of each cb_id only.
        {
            static uint32_t seen_mask;
            if (cb_id < 32 && !(seen_mask & (1u << cb_id))) {
                seen_mask |= (1u << cb_id);
                ESP_LOGI(TAG, "first zcl cb_id 0x%x", (unsigned)cb_id);
            }
        }
        break;
    }

    if (evt[0]) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        record_event_locked_first_active(evt);
        xSemaphoreGive(s_mtx);
        webhook_send(s_last_src, evt);
    }
    return ESP_OK;
}

// Periodic heartbeat so the serial log proves the board is alive even with
// nothing happening on the Zigbee side.
static void zb_heartbeat_task(void *arg)
{
    while (true) {
        zb_ui_status_t st;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        st = s_status;
        xSemaphoreGive(s_mtx);
        ESP_LOGI(TAG, "hb: state=%d pan=%04x ch=%u devices=%d permit=%s/%lus",
                 st.state, st.pan_id, st.channel, st.num_devices,
                 st.permit_open ? "OPEN" : "closed",
                 (unsigned long)st.permit_remaining_s);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

// --- BDB / ZDO signal handler ---
void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;

    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "stack init; enabling Wi-Fi/802.15.4 coex then Wi-Fi");
        // The 802.15.4 platform is up now. Enable the coex arbiter BEFORE
        // bringing Wi-Fi up so the STA gets RX windows during association and
        // keeps hearing beacons afterwards (otherwise: reason-200 timeout).
        ESP_ERROR_CHECK(esp_coex_wifi_i154_enable());
        wifi_task_prepare();
        wifi_task_connect();
        // Minimum modem-sleep lets the coex arbiter time-slice the radio to
        // 802.15.4. Required for stable coexistence with the coordinator.
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_status.state = ZB_UI_STARTING;
        xSemaphoreGive(s_mtx);
        ESP_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(
            ESP_ZB_BDB_MODE_INITIALIZATION));
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            s_status.state = ZB_UI_FORMING;
            xSemaphoreGive(s_mtx);
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "factory new, forming network");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
            } else {
                ESP_LOGI(TAG, "rebooted on existing network");
                esp_zb_ieee_addr_t ext;
                esp_zb_get_long_address(ext);
                xSemaphoreTake(s_mtx, portMAX_DELAY);
                s_status.state      = ZB_UI_READY;
                s_status.pan_id     = esp_zb_get_pan_id();
                s_status.short_addr = esp_zb_get_short_address();
                s_status.channel    = esp_zb_get_current_channel();
                memcpy(s_status.ext_addr, ext, sizeof(ext));
                xSemaphoreGive(s_mtx);
                esp_zb_bdb_open_network(PERMIT_JOIN_SECS);
                ESP_LOGI(TAG, "F&B target on ep %d for %ds", COORD_ENDPOINT, PERMIT_JOIN_SECS);
                esp_zb_bdb_finding_binding_start_target(COORD_ENDPOINT, PERMIT_JOIN_SECS);
            }
        } else {
            ESP_LOGW(TAG, "start failed (%d), retrying", err_status);
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            s_status.state = ZB_UI_ERROR;
            xSemaphoreGive(s_mtx);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_FORMATION:
        if (err_status == ESP_OK) {
            esp_zb_ieee_addr_t ext;
            esp_zb_get_long_address(ext);
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            s_status.state      = ZB_UI_READY;
            s_status.pan_id     = esp_zb_get_pan_id();
            s_status.short_addr = esp_zb_get_short_address();
            s_status.channel    = esp_zb_get_current_channel();
            memcpy(s_status.ext_addr, ext, sizeof(ext));
            xSemaphoreGive(s_mtx);
            ESP_LOGI(TAG, "network formed: PAN 0x%04x ch %d, opening permit-join %ds",
                     s_status.pan_id, s_status.channel, PERMIT_JOIN_SECS);
            esp_zb_bdb_open_network(PERMIT_JOIN_SECS);
            // Also advertise this endpoint as a Find&Bind target so STYRBAR's
            // initiator step finds us and creates a binding to our clusters.
            ESP_LOGI(TAG, "F&B target on ep %d for %ds", COORD_ENDPOINT, PERMIT_JOIN_SECS);
            esp_zb_bdb_finding_binding_start_target(COORD_ENDPOINT, PERMIT_JOIN_SECS);
        } else {
            ESP_LOGW(TAG, "formation failed (%d), retrying in 1s", err_status);
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                                   ESP_ZB_BDB_MODE_NETWORK_FORMATION, 1000);
        }
        break;

    case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        // Payload is a single byte: remaining seconds (0 = closed).
        uint8_t *p = (uint8_t *)esp_zb_app_signal_get_params(p_sg_p);
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_status.permit_open = (p && *p > 0);
        s_status.permit_remaining_s = p ? *p : 0;
        xSemaphoreGive(s_mtx);
        if (p) ESP_LOGI(TAG, "permit-join %s (%us)",
                        *p ? "OPEN" : "CLOSED", *p);
        break;
    }

    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        esp_zb_zdo_signal_device_annce_params_t *p =
            (esp_zb_zdo_signal_device_annce_params_t *)
            esp_zb_app_signal_get_params(p_sg_p);
        if (p) {
            ESP_LOGI(TAG, "device joined: short=0x%04x ieee=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                     p->device_short_addr,
                     p->ieee_addr[7], p->ieee_addr[6], p->ieee_addr[5], p->ieee_addr[4],
                     p->ieee_addr[3], p->ieee_addr[2], p->ieee_addr[1], p->ieee_addr[0]);
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            device_find_or_alloc_locked(p->device_short_addr, p->ieee_addr);
            xSemaphoreGive(s_mtx);
            devices_save();

            // IKEA STYRBAR arrow buttons emit MFG-specific TradfriArrow commands
            // on Scenes cluster (0x0005) that F&B doesn't auto-bind. Explicitly
            // bind STYRBAR's endpoint 1 / Scenes -> us so arrows reach our
            // action handler. We also bind 0x0006 / 0x0008 as a belt-and-braces
            // (F&B should have done these already).
            esp_zb_ieee_addr_t our_ieee;
            esp_zb_get_long_address(our_ieee);
            const uint16_t clusters_to_bind[] = { 0x0005, 0x0006, 0x0008 };
            for (size_t i = 0; i < sizeof(clusters_to_bind)/sizeof(clusters_to_bind[0]); i++) {
                esp_zb_zdo_bind_req_param_t bind_req = {
                    .src_endp = 1,
                    .cluster_id = clusters_to_bind[i],
                    .dst_addr_mode = ESP_ZB_ZDO_BIND_DST_ADDR_MODE_64_BIT_EXTENDED,
                    .dst_endp = COORD_ENDPOINT,
                    .req_dst_addr = p->device_short_addr,
                };
                memcpy(bind_req.src_address, p->ieee_addr, 8);
                memcpy(bind_req.dst_address_u.addr_long, our_ieee, 8);
                esp_zb_zdo_device_bind_req(&bind_req, NULL, NULL);
            }
            ESP_LOGI(TAG, "sent bind reqs for 0x0005/0x0006/0x0008 to 0x%04x",
                     p->device_short_addr);
        }
        break;
    }

    case ESP_ZB_ZDO_SIGNAL_LEAVE_INDICATION: {
        esp_zb_zdo_signal_leave_indication_params_t *p =
            (esp_zb_zdo_signal_leave_indication_params_t *)
            esp_zb_app_signal_get_params(p_sg_p);
        if (p) {
            ESP_LOGI(TAG, "device leave: short=0x%04x", p->short_addr);
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            for (int i = 0; i < ZB_MAX_DEVICES; i++) {
                if (s_status.devices[i].active &&
                    s_status.devices[i].short_addr == p->short_addr) {
                    s_status.devices[i].active = false;
                    s_status.num_devices--;
                    break;
                }
            }
            xSemaphoreGive(s_mtx);
            devices_save();
        }
        break;
    }

    default:
        ESP_LOGD(TAG, "signal %s (0x%x), status %s",
                 esp_zb_zdo_signal_to_string(sig_type), sig_type,
                 esp_err_to_name(err_status));
        break;
    }
}

// --- main task ---
static void zb_main_task(void *arg)
{
    esp_zb_cfg_t cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_COORDINATOR,
        .install_code_policy = false,
        .nwk_cfg.zczr_cfg = {
            .max_children = ZB_MAX_DEVICES,
        },
    };
    esp_zb_init(&cfg);

    // Build a single endpoint that advertises the SERVER side of the clusters
    // STYRBAR (Find&Bind initiator) is looking for.
    esp_zb_cluster_list_t *cl = esp_zb_zcl_cluster_list_create();

    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version  = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = 0x01, // mains
    };
    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(&basic_cfg);
    esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                                  (void *)MFR_NAME);
    esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
                                  (void *)MODEL_NAME);
    esp_zb_cluster_list_add_basic_cluster(cl, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_identify_cluster_cfg_t ident_cfg = { .identify_time = 0 };
    esp_zb_cluster_list_add_identify_cluster(cl,
        esp_zb_identify_cluster_create(&ident_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_on_off_cluster_cfg_t onoff_cfg = { .on_off = false };
    esp_zb_cluster_list_add_on_off_cluster(cl,
        esp_zb_on_off_cluster_create(&onoff_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_level_cluster_cfg_t lvl_cfg = { .current_level = 0 };
    esp_zb_cluster_list_add_level_cluster(cl,
        esp_zb_level_cluster_create(&lvl_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_scenes_cluster_cfg_t scn_cfg = {
        .scenes_count = 0, .current_scene = 0, .current_group = 0,
        .scene_valid = false, .name_support = 0,
    };
    esp_zb_cluster_list_add_scenes_cluster(cl,
        esp_zb_scenes_cluster_create(&scn_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_groups_cluster_cfg_t grp_cfg = { .groups_name_support_id = 0 };
    esp_zb_cluster_list_add_groups_cluster(cl,
        esp_zb_groups_cluster_create(&grp_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint = COORD_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id  = ESP_ZB_HA_ON_OFF_LIGHT_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_ep_list_add_ep(ep_list, cl, ep_cfg);

    esp_zb_device_register(ep_list);
    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_raw_command_handler_register(zb_raw_handler);
    // Lock Zigbee to 802.15.4 channel 25 (2475 MHz). This sits above the
    // Wi-Fi 2.4 GHz band so it doesn't conflict with any Wi-Fi channel the
    // router might pick on auto. (Channel 26 = 2480 MHz also works but
    // requires FCC regulatory clearance.)
    esp_zb_set_primary_network_channel_set(1u << 25);

    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

void zigbee_task_start(void)
{
    s_mtx = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(nvs_flash_init());
    devices_load();

    // Needed by Wi-Fi (brought up later from the Zigbee SKIP_STARTUP signal).
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // PM framework must be active for the Wi-Fi/802.15.4 coex arbiter to
    // time-slice the single radio. light_sleep_enable stays false: a Zigbee
    // coordinator must keep its 802.15.4 RX on continuously.
    esp_pm_config_t pm = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 80,
        .light_sleep_enable = false,
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm));

    esp_zb_platform_config_t plat = {
        .radio_config = { .radio_mode = ZB_RADIO_MODE_NATIVE },
        .host_config  = { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE },
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&plat));

    webhook_start();
    xTaskCreate(zb_main_task, "zb_main", 8192, NULL, 5, NULL);
    xTaskCreate(zb_heartbeat_task, "zb_hb", 3072, NULL, 3, NULL);
}
