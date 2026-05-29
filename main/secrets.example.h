#pragma once

// Template for local secrets. Copy this file to secrets.h and fill in your
// own values. secrets.h is gitignored so credentials never leave your machine.

#define WIFI_SSID    "your-wifi-ssid"
#define WIFI_PASS    "your-wifi-password"

// Webhook listener on your LAN (see tools/webhook_listener.py). Point this at
// the host running the listener, e.g. "http://192.168.1.50:8899/event".
#define WEBHOOK_URL  "http://CHANGE-ME:8899/event"
