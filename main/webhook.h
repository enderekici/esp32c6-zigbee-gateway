#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start the webhook worker (queue + task). Call once at boot.
void webhook_start(void);

// Enqueue a button event to POST to the configured listener. Safe to call
// from Zigbee callbacks — never blocks (drops if queue full).
void webhook_send(uint16_t short_addr, const char *event);

#ifdef __cplusplus
}
#endif
