#include "wifi_task.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"

#include "http_task.h"
#include "secrets.h"

static const char *TAG = "wifi";

static SemaphoreHandle_t s_mtx;
static wifi_ui_status_t  s_status;

void wifi_task_get_status(wifi_ui_status_t *out)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_mtx);
}

static void set_state(wifi_ui_state_t st)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_status.state = st;
    xSemaphoreGive(s_mtx);
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        set_state(WIFI_UI_CONNECTING);
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "disconnected (reason %d), reconnecting", d ? d->reason : -1);
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_status.state = WIFI_UI_DISCONNECTED;
        s_status.ip[0] = 0;
        s_status.rssi  = 0;
        xSemaphoreGive(s_mtx);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_status.state = WIFI_UI_CONNECTED;
        snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&e->ip_info.ip));
        xSemaphoreGive(s_mtx);
        ESP_LOGI(TAG, "got IP %s", s_status.ip);
        static bool http_started = false;
        if (!http_started) {
            http_started = true;
            http_task_start();
        }
    }
}

static void rssi_task(void *arg)
{
    while (true) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            s_status.rssi = ap.rssi;
            xSemaphoreGive(s_mtx);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void wifi_task_prepare(void)
{
    s_mtx = xSemaphoreCreateMutex();
    s_status.state = WIFI_UI_BOOTING;

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Hand coex the Wi-Fi power-save schedule so it preempts 802.15.4 RX
    // before the AP beacon window is missed (guards against reason-200).
    esp_wifi_coex_pwr_configure(true);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL, NULL));

    wifi_config_t wc = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .capable = true, .required = false },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    xTaskCreate(rssi_task, "wifi_rssi", 3072, NULL, 3, NULL);
}

void wifi_task_connect(void)
{
    ESP_ERROR_CHECK(esp_wifi_start());
    set_state(WIFI_UI_CONNECTING);
}
