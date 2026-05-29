#include "http_task.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "zigbee_task.h"
#include "wifi_task.h"

static const char *TAG = "http";

static const char *state_name(zb_ui_state_t s)
{
    switch (s) {
    case ZB_UI_BOOTING:  return "BOOT";
    case ZB_UI_STARTING: return "INIT";
    case ZB_UI_FORMING:  return "FORMING";
    case ZB_UI_READY:    return "READY";
    default:             return "ERROR";
    }
}

// GET /api/status  -> JSON snapshot of the gateway + devices.
static esp_err_t api_status_handler(httpd_req_t *req)
{
    zb_ui_status_t st;
    zigbee_task_get_status(&st);
    wifi_ui_status_t w;
    wifi_task_get_status(&w);
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000LL);

    char *buf = malloc(4096);
    if (!buf) return httpd_resp_send_500(req);
    int n = 0;
    n += snprintf(buf + n, 4096 - n,
        "{\"uptime_s\":%lu,\"wifi\":{\"ip\":\"%s\",\"rssi\":%d},"
        "\"zigbee\":{\"state\":\"%s\",\"pan_id\":\"0x%04x\",\"channel\":%u,"
        "\"permit_open\":%s,\"permit_remaining_s\":%lu,\"num_devices\":%d},"
        "\"heap\":{\"free\":%lu,\"min_free\":%lu,\"largest_block\":%lu},"
        "\"devices\":[",
        (unsigned long)up, w.ip, w.rssi, state_name(st.state), st.pan_id,
        st.channel, st.permit_open ? "true" : "false",
        (unsigned long)st.permit_remaining_s, st.num_devices,
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)esp_get_minimum_free_heap_size(),
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

    bool first = true;
    for (int i = 0; i < ZB_MAX_DEVICES; i++) {
        if (!st.devices[i].active) continue;
        const zb_device_t *d = &st.devices[i];
        n += snprintf(buf + n, 4096 - n,
            "%s{\"short\":\"0x%04x\",\"ieee\":\"%02x%02x%02x%02x%02x%02x%02x%02x\","
            "\"last_event\":\"%s\",\"last_event_age_s\":%lu}",
            first ? "" : ",", d->short_addr,
            d->ieee_addr[7], d->ieee_addr[6], d->ieee_addr[5], d->ieee_addr[4],
            d->ieee_addr[3], d->ieee_addr[2], d->ieee_addr[1], d->ieee_addr[0],
            d->last_event[0] ? d->last_event : "",
            (unsigned long)(d->last_event_s ? up - d->last_event_s : 0));
        first = false;
    }
    n += snprintf(buf + n, 4096 - n, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, buf, n);
    free(buf);
    return ESP_OK;
}

// POST /api/permit  -> re-open permit-join for 180s.
static esp_err_t api_permit_handler(httpd_req_t *req)
{
    zigbee_task_open_network(180);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static const char INDEX_HTML[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>C6 Zigbee GW</title><style>"
"body{font-family:ui-monospace,Menlo,monospace;background:#0b0e14;color:#d6deeb;margin:0;padding:16px}"
"h1{font-size:18px;color:#5ccfe6;margin:0 0 4px}"
".sub{color:#7a88a0;font-size:12px;margin-bottom:16px}"
".card{background:#131722;border:1px solid #222838;border-radius:8px;padding:12px 14px;margin-bottom:10px}"
".row{display:flex;justify-content:space-between;font-size:13px;padding:2px 0}"
".k{color:#7a88a0}.v{color:#d6deeb}"
".ev{color:#7ee787;font-weight:bold}.age{color:#7a88a0}"
".pill{display:inline-block;padding:2px 8px;border-radius:10px;font-size:11px}"
".ok{background:#10331c;color:#7ee787}.warn{background:#3a2f10;color:#ffd479}.bad{background:#3a1414;color:#ff8a8a}"
"button{background:#1b2335;color:#5ccfe6;border:1px solid #2a3450;border-radius:6px;padding:6px 12px;font:inherit;cursor:pointer}"
"</style></head><body>"
"<h1>C6 ZIGBEE GATEWAY</h1><div class=sub id=sub>loading...</div>"
"<div id=gw class=card></div>"
"<div style='display:flex;justify-content:space-between;align-items:center;margin:14px 0 6px'>"
"<b style='color:#5ccfe6'>DEVICES</b><button onclick=permit()>Open join 180s</button></div>"
"<div id=devs></div>"
"<script>"
"function pill(t,c){return '<span class=\"pill '+c+'\">'+t+'</span>'}"
"async function permit(){await fetch('/api/permit',{method:'POST'});load()}"
"async function load(){"
"try{let r=await fetch('/api/status');let d=await r.json();"
"let z=d.zigbee,w=d.wifi;"
"let sc=z.state=='READY'?'ok':(z.state=='ERROR'?'bad':'warn');"
"document.getElementById('sub').textContent='uptime '+d.uptime_s+'s';"
"document.getElementById('gw').innerHTML="
"'<div class=row><span class=k>Zigbee</span><span class=v>'+pill(z.state,sc)+'</span></div>'+"
"'<div class=row><span class=k>PAN / channel</span><span class=v>'+z.pan_id+' / ch'+z.channel+'</span></div>'+"
"'<div class=row><span class=k>Permit join</span><span class=v>'+(z.permit_open?pill(z.permit_remaining_s+'s','warn'):pill('closed','bad'))+'</span></div>'+"
"'<div class=row><span class=k>Wi-Fi</span><span class=v>'+(w.ip?pill(w.ip+' '+w.rssi+'dBm','ok'):pill('down','bad'))+'</span></div>';"
"let h='';if(!d.devices.length)h='<div class=card style=color:#7a88a0>no devices paired</div>';"
"for(let v of d.devices){h+='<div class=card><div class=row><span class=v>'+v.short+'</span>"
"<span class=k>'+v.ieee+'</span></div>'+"
"(v.last_event?'<div class=row><span class=ev>'+v.last_event+'</span><span class=age>'+v.last_event_age_s+'s ago</span></div>':'')+'</div>'}"
"document.getElementById('devs').innerHTML=h;"
"}catch(e){document.getElementById('sub').textContent='offline'}}"
"load();setInterval(load,1500);"
"</script></body></html>";

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, sizeof(INDEX_HTML) - 1);
    return ESP_OK;
}

void http_task_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return;
    }
    httpd_uri_t index = { .uri = "/", .method = HTTP_GET, .handler = index_handler };
    httpd_uri_t status = { .uri = "/api/status", .method = HTTP_GET, .handler = api_status_handler };
    httpd_uri_t permit = { .uri = "/api/permit", .method = HTTP_POST, .handler = api_permit_handler };
    httpd_register_uri_handler(server, &index);
    httpd_register_uri_handler(server, &status);
    httpd_register_uri_handler(server, &permit);
    ESP_LOGI(TAG, "dashboard up on http://<ip>/");
}
