# ESP32-C6 Zigbee Gateway + LVGL Dashboard

A standalone, battery-friendly home-automation bridge on a single
**Waveshare ESP32-C6-LCD-1.47** board. The ESP32-C6 runs as a **Zigbee
coordinator** *and* a **Wi-Fi station at the same time** on its single 2.4 GHz
radio, pairs an **IKEA STYRBAR** remote, and turns button presses into HTTP
webhooks that trigger actions on your PC. A live dashboard is shown both on the
onboard 1.47" LCD (via LVGL) and over HTTP in a browser.

No USB host, no hub, no cloud — the board does the coordinator, the Wi-Fi
uplink, and the UI by itself.

```
  IKEA STYRBAR  ──802.15.4/Zigbee──▶  ESP32-C6 (coordinator)  ──Wi-Fi/HTTP──▶  PC webhook listener
                                          │                                        │
                                          ├─▶ LCD dashboard (LVGL)                 └─▶ runs a shell command
                                          └─▶ HTTP dashboard + JSON API
```

## Features

- **Zigbee coordinator** (esp-zigbee-sdk, ZCZR) on the C6's native 802.15.4 radio — forms its own network, no external hub.
- **Simultaneous Wi-Fi STA** sharing the one radio via the coexistence arbiter (the hard part — see [Wi-Fi / Zigbee coexistence](#wi-fi--zigbee-coexistence)).
- **IKEA STYRBAR decoding** — all four buttons + long-press/release, including the manufacturer-specific arrow commands that don't show up as standard ZCL writes.
- **Webhook bridge** — every button press POSTs `{"device","event"}` to a listener on your LAN, which maps events to arbitrary shell commands.
- **LVGL dashboard** on the LCD — gateway state, Wi-Fi, paired devices, last event, uptime.
- **Web dashboard + JSON API** — same data in a browser, plus a one-click "open join" button.
- **Device list persisted to NVS** — paired devices survive a reflash/reboot.

## Hardware

- **Board:** Waveshare ESP32-C6-LCD-1.47 (ESP32-C6FH8: Wi-Fi 6 + BLE + 802.15.4, 8 MB flash, **no PSRAM**).
- **Display:** ST7789T, 172×320 IPS, SPI. Visible window is centered in the controller's 240-wide RAM → **column offset 34, row offset 0**.
- **Onboard RGB LED:** WS2812 on GPIO8 (cleared at boot so the board isn't glowing).
- **Remote:** IKEA STYRBAR (also works as a reference for other Tradfri remotes).

### Pinout

| Function | GPIO |
|---|---|
| LCD MOSI | 6 |
| LCD SCLK | 7 |
| LCD CS   | 14 |
| LCD DC   | 15 |
| LCD RST  | 21 |
| LCD backlight (PWM) | 22 |
| WS2812 RGB LED | 8 |

SPI host `SPI2_HOST` @ 10 MHz, backlight via LEDC PWM at ~30 % duty (cooler, still readable indoors).

## Repository layout

```
main/
  main.c            boot: WS2812 off, LCD init, Zigbee task, LVGL UI
  zigbee_task.c     coordinator: network formation, permit-join, STYRBAR decode, NVS device list
  wifi_task.c       Wi-Fi STA + coexistence enable + status snapshot
  webhook.c         FreeRTOS queue + worker → HTTP POST on each event
  http_task.c       esp_http_server: web dashboard + /api/status + /api/permit
  ui_lvgl.c         LVGL dashboard (primary UI)
  lcd.c             esp_lcd ST7789 setup + hand-rolled Spleen-font blitter (legacy UI)
  Vernon_ST7789T.c  custom esp_lcd panel driver (honors set_gap for the 34px offset)
  font_spleen.c     8×16 bitmap font for the legacy UI
  board.h           pin + geometry constants
  secrets.example.h template for Wi-Fi creds + webhook host (copy to secrets.h)
tools/
  webhook_listener.py   Python listener that maps events → shell commands
partitions.csv      adds zb_storage + zb_fct partitions required by esp-zigbee-sdk
sdkconfig.defaults  target, flash, Zigbee role, coexistence, LVGL fonts
```

## Build, flash, run

### Prerequisites

- **ESP-IDF v5.4** (manifest pins `>=5.3,<5.5`). Install the target once:
  ```bash
  ~/esp/esp-idf-v5.4/install.sh esp32c6
  ```
- Managed components are pulled automatically by the IDF component manager:
  `espressif/esp-zigbee-lib`, `espressif/esp-zboss-lib`, `espressif/esp_lvgl_port`, `lvgl/lvgl` (9.5), `espressif/led_strip`.

### 1. Configure secrets (required)

Credentials are **not** committed. Copy the template and fill in your values:

```bash
cp main/secrets.example.h main/secrets.h
$EDITOR main/secrets.h
```

```c
#define WIFI_SSID    "your-wifi-ssid"
#define WIFI_PASS    "your-wifi-password"
#define WEBHOOK_URL  "http://192.168.1.50:8899/event"   // host running the listener
```

`main/secrets.h` is gitignored.

### 2. Build and flash

```bash
. ~/esp/esp-idf-v5.4/export.sh
idf.py set-target esp32c6     # first time only
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Exit the monitor with `Ctrl+]`. Find the port with `ls /dev/cu.usbmodem*`.

> If you change `sdkconfig.defaults` (e.g. LVGL fonts), delete `sdkconfig` so the
> defaults regenerate: `rm sdkconfig && idf.py build`.

## Pairing a STYRBAR

1. Boot the board — it forms a network and opens **permit-join for 180 s** automatically.
2. Put the STYRBAR into pairing mode (hold the pair button ~4× until the LED blinks; consult IKEA's instructions for your firmware).
3. The remote joins, a Find & Bind binds its clusters to the coordinator, and it appears on both dashboards.
4. To re-open join later, press **"Open join 180s"** on the web dashboard, or `POST /api/permit`.

Paired devices are saved to NVS (namespace `gw`), so they persist across reboots and reflashes.

## STYRBAR button → event mapping

The firmware emits these event strings (decoded across three ZCL clusters; the
arrow commands are manufacturer-specific and are caught by a raw-ZCL handler):

| Button / gesture | Event string | Source cluster |
|---|---|---|
| Left "sun" (bright) press | `BRIGHT` | On/Off `0x0006` |
| Right "sun" (dim) press | `DIM` | On/Off `0x0006` |
| Left long-press | `BRIGHT HLD` | Level `0x0008` |
| Right long-press | `DIM HLD` | Level `0x0008` |
| Bright/dim release | `BRT REL` | Level `0x0008` |
| Arrow up / down | `ARR LEFT` / `ARR RIGHT` | Scenes `0x0005` (mfg-specific) |
| Arrow long-press | `ARR LEFT HLD` / `ARR RIGHT HLD` | Scenes `0x0005` |
| Arrow release | `ARR REL` | Scenes `0x0005` |

> The On/Off (`BRIGHT`/`DIM`) events arrive via a SET_ATTR callback that carries
> **no source address**, so the device's short address is stashed from the raw
> frame that passes through first. That's why a `s_last_src` cache exists in
> `zigbee_task.c`.

## Webhook listener (the PC side)

`tools/webhook_listener.py` is a tiny stdlib HTTP server (port 8899) that maps
event names to shell commands:

```python
ACTIONS = {
    "BRIGHT":    "echo BRIGHT pressed",   # e.g. "cd ~/proj && ./deploy.sh"
    "DIM":       "echo DIM pressed",
    "ARR LEFT":  "echo previous",
    "ARR RIGHT": "echo next",
    # ... unmapped / empty events are ignored
}
```

Run it on the host named in `WEBHOOK_URL` (use `-u` for unbuffered live logs):

```bash
python3 -u tools/webhook_listener.py
```

For unattended use, run it under `launchd` (macOS) / `systemd` (Linux) so it
survives logout. It listens on `0.0.0.0:8899`.

## HTTP API

Served by `esp_http_server` on port 80 (the board's Wi-Fi IP). CORS is open
(`Access-Control-Allow-Origin: *`).

| Method & path | Purpose |
|---|---|
| `GET /` | HTML dashboard (auto-refreshes every 1.5 s) |
| `GET /api/status` | JSON snapshot — see below |
| `POST /api/permit` | Re-open permit-join for 180 s |

`GET /api/status` returns:

```json
{
  "uptime_s": 1234,
  "wifi":   { "ip": "192.168.x.y", "rssi": -60 },
  "zigbee": { "state": "READY", "pan_id": "0x8725", "channel": 25,
              "permit_open": true, "permit_remaining_s": 180, "num_devices": 1 },
  "heap":   { "free": 116300, "min_free": 114544, "largest_block": 88064 },
  "devices": [ { "short": "0xec3e", "ieee": "f074bffffe19a7ef",
                 "last_event": "BRIGHT", "last_event_age_s": 3 } ]
}
```

## Wi-Fi / Zigbee coexistence

The C6 has **one** 2.4 GHz radio shared between Wi-Fi and 802.15.4. A Zigbee
**coordinator** keeps its receiver on continuously, which starves Wi-Fi of
receive windows and triggers an AP beacon timeout (**disconnect reason 200**).
The fix is to let the coex arbiter, driven by the power-management / modem-sleep
framework, time-slice the radio. This requires **both** build config and runtime
calls:

`sdkconfig.defaults`:
```ini
CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y
CONFIG_ESP_COEX_POWER_MANAGEMENT=y
CONFIG_PM_ENABLE=y
CONFIG_ESP_WIFI_STA_DISCONNECTED_PM_ENABLE=y
CONFIG_FREERTOS_HZ=1000          # Zigbee stack prefers a 1 kHz tick
```

Runtime calls:
```c
esp_wifi_coex_pwr_configure(true);   // wifi_task.c — hand coex the Wi-Fi PS schedule
esp_coex_wifi_i154_enable();         // zigbee_task.c — enable Wi-Fi/802.15.4 coex
esp_wifi_set_ps(WIFI_PS_MIN_MODEM);  // zigbee_task.c — modem-sleep so coex can time-slice
```

Without all of these, the coordinator's always-on RX wins the radio and Wi-Fi
keeps dropping. This is the single trickiest part of the project and has no
ready-made reference — most ESP32-C6 examples do Wi-Fi *or* Zigbee, not both.

## Memory & flash budget

No PSRAM — everything lives in the C6's ~512 KB HP SRAM.

- **Heap with LVGL running:** ~116 KB free, ~88–90 KB largest contiguous block, `min_free` steady (no exhaustion). LVGL + double partial draw buffers + Montserrat fonts + RMT cost ~100 KB vs. the bitmap-blitter UI.
- **App partition:** `factory` is 1900 KB; the image uses ~95 % of it. Headroom is thin — watch the size if you add features.
- `partitions.csv` adds the `zb_storage` (16 K FAT) and `zb_fct` (1 K FAT) partitions the Zigbee stack requires for network/binding state.

## UI notes (LCD)

- LVGL 9.5 via `esp_lvgl_port`, primary UI (`USE_LVGL_UI` in `main.c`). The original hand-rolled Spleen-font blitter UI is kept behind `#if !USE_LVGL_UI` for reference / no-LVGL builds.
- Geometry settled at 172×320 with `set_gap(34, 0)` applied at the panel level, so both the blitter and LVGL flushes land aligned. Independently confirmed against VolosR's WaveShare C6 example.
- Two display gotchas, both documented inline in `ui_lvgl.c`:
  - `esp_lvgl_port` re-applies the panel mirror from its rotation flags during `add_disp`, overwriting `lcd_init`'s `mirror(true)` → set `rotation.mirror_x = true` to keep the working orientation (else the whole UI renders mirrored).
  - `swap_bytes` must be **false** on this LVGL9 path; leaving it `true` double-swaps RGB565 bytes (dark navy renders as magenta, dark borders as green).

## Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| Wi-Fi keeps disconnecting (reason 200) | Coexistence not fully enabled — verify the `sdkconfig` flags and runtime calls above. |
| Webhook never fires | Nothing listening on `WEBHOOK_URL`; start `webhook_listener.py` on that host and check the LAN IP/port. |
| `BRIGHT`/`DIM` show device `0x0000` | Old build — the source address is now recovered from the raw frame (`s_last_src`). |
| LCD shows mirrored text | `rotation.mirror_x` must be `true` (see UI notes). |
| LCD colors look wrong (magenta/green tint) | `swap_bytes` must be `false` (see UI notes). |
| Build can't find Wi-Fi/webhook macros | You didn't create `main/secrets.h` from the template. |

## License / status

Personal hobby project, provided as-is. The Zigbee + Wi-Fi coexistence
configuration in particular may need tuning for a different AP, channel, or
board revision.
