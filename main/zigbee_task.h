#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZB_MAX_DEVICES 16

typedef enum {
    ZB_UI_BOOTING = 0,
    ZB_UI_STARTING,
    ZB_UI_FORMING,
    ZB_UI_READY,        // coordinator running, network formed
    ZB_UI_ERROR,
} zb_ui_state_t;

typedef struct {
    bool     active;
    uint16_t short_addr;
    uint8_t  ieee_addr[8];
    uint32_t joined_s;          // uptime in seconds when device joined
    uint32_t last_event_s;      // uptime when last event seen
    char     last_event[24];    // e.g. "BRIGHT" / "DIM" / "UP" / "DOWN"
    uint8_t  last_level;        // for direction-detection on Level Control
} zb_device_t;

typedef struct {
    zb_ui_state_t state;
    uint16_t      pan_id;
    uint16_t      short_addr;
    uint8_t       channel;
    uint8_t       ext_addr[8];
    bool          permit_open;
    uint32_t      permit_remaining_s;
    int           num_devices;
    zb_device_t   devices[ZB_MAX_DEVICES];
} zb_ui_status_t;

void zigbee_task_start(void);
void zigbee_task_get_status(zb_ui_status_t *out);

// Re-open permit-join for `duration_s` seconds (max 254).
void zigbee_task_open_network(uint8_t duration_s);

#ifdef __cplusplus
}
#endif
