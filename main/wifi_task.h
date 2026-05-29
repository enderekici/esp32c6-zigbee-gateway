#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_UI_BOOTING = 0,
    WIFI_UI_CONNECTING,
    WIFI_UI_CONNECTED,
    WIFI_UI_DISCONNECTED,
} wifi_ui_state_t;

typedef struct {
    wifi_ui_state_t state;
    char            ip[16];
    int8_t          rssi;
} wifi_ui_status_t;

// One-time: init netif + esp_wifi + STA config + event handlers. Does NOT
// connect. Must run after esp_netif_init() + esp_event_loop_create_default()
// and (for coex) after the 802.15.4 platform is up. Safe to call from the
// Zigbee SKIP_STARTUP signal.
void wifi_task_prepare(void);

// Start the STA and begin connecting (auto-reconnect on disconnect).
void wifi_task_connect(void);

void wifi_task_get_status(wifi_ui_status_t *out);

#ifdef __cplusplus
}
#endif
