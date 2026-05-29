#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Start the dashboard HTTP server. Call once Wi-Fi has an IP.
void http_task_start(void);

#ifdef __cplusplus
}
#endif
