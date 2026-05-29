# ESP32-C6 + 1.47" LCD + Zigbee hello-world

Target: **Waveshare ESP32-C6-LCD-1.47** (ESP32-C6FH8, 8 MB flash, ST7789 172×320).

Acts as a Zigbee End Device (HA on/off light) and renders network status on the LCD.

## Pins

| Function | GPIO |
|---|---|
| LCD MOSI | 6 |
| LCD SCLK | 7 |
| LCD CS   | 14 |
| LCD DC   | 15 |
| LCD RST  | 21 |
| LCD BL   | 22 |

Display offset: column 34, row 0 (172 wide centered in 240-wide ST7789 RAM).

## One-time setup

```bash
# Toolchain (already started in this session under ~/esp/esp-idf)
~/esp/esp-idf/install.sh esp32c6

# Per-shell
. ~/esp/esp-idf/export.sh
```

## Build / flash / monitor

```bash
cd ~/code/esp32c6
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/cu.usbmodem21101 flash monitor
```

Exit monitor with `Ctrl+]`.

## What you should see

- Boot: "Booting…" then "ESP32-C6 + ZB" header
- `ZB: SEARCHING` (yellow) while it scans 802.15.4 channels for an open network
- `ZB: JOINED` (green) when a coordinator (Home Assistant ZHA / Zigbee2MQTT / deCONZ) accepts the join, then PAN ID, network short address, channel, and EUI64

To test joining: put your coordinator into permit-join mode within ~3 minutes of boot.

## Architecture

- `main/main.c` — boots LCD then spawns Zigbee task + UI render task
- `main/lcd.c` — esp_lcd SPI + ST7789, 8x8 bitmap font text
- `main/zigbee_task.c` — esp-zigbee-sdk end-device, on/off light endpoint, status snapshot for UI
- `main/board.h` — Waveshare pinout constants
- `partitions.csv` — adds `zb_storage` + `zb_fct` FAT partitions required by esp-zigbee-sdk
- `sdkconfig.defaults` — pins target, flash size, Zigbee end-device role, native 802.15.4 radio
