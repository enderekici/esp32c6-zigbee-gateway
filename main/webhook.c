#include "webhook.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_http_client.h"

#include "secrets.h"

static const char *TAG = "webhook";

// WEBHOOK_URL is defined in secrets.h (gitignored). It points at the host
// running the listener on your LAN — see tools/webhook_listener.py.

typedef struct {
    uint16_t short_addr;
    char     event[24];
} webhook_evt_t;

static QueueHandle_t s_queue;

void webhook_send(uint16_t short_addr, const char *event)
{
    if (!s_queue || !event || !event[0]) return;
    webhook_evt_t e = { .short_addr = short_addr };
    snprintf(e.event, sizeof(e.event), "%s", event);
    xQueueSend(s_queue, &e, 0);  // non-blocking; drop if full
}

static void worker(void *arg)
{
    webhook_evt_t e;
    char body[96];
    while (true) {
        if (xQueueReceive(s_queue, &e, portMAX_DELAY) != pdTRUE) continue;
        int n = snprintf(body, sizeof(body),
                         "{\"device\":\"0x%04x\",\"event\":\"%s\"}",
                         e.short_addr, e.event);
        esp_http_client_config_t cfg = {
            .url = WEBHOOK_URL,
            .method = HTTP_METHOD_POST,
            .timeout_ms = 3000,
        };
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_post_field(c, body, n);
        esp_err_t err = esp_http_client_perform(c);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "POST %s -> %d", body, esp_http_client_get_status_code(c));
        } else {
            ESP_LOGW(TAG, "POST %s failed: %s", body, esp_err_to_name(err));
        }
        esp_http_client_cleanup(c);
    }
}

void webhook_start(void)
{
    s_queue = xQueueCreate(8, sizeof(webhook_evt_t));
    xTaskCreate(worker, "webhook", 4096, NULL, 4, NULL);
}
