# ESP32 WinKeyer

WinKeyer (K1EL WK3 protocol) clone on ESP32 — WiFi TCP serial bridge to the PC,
iambic paddle keying, sidetone, speed pot. WinKeyer protocol by Steve K1EL;
K3ng keyer by Anthony Good K3NG used as reference.

ESP firmware built with PlatformIO. Default env `esp32-winkeyer` targets a
classic ESP32 devkit (`esp32dev`, CP2102); env `esp32s3-winkeyer` is the
future ESP32-S3 build with native USB CDC. WiFi is onboarded via a
WiFiManager captive portal; state is reported to the shack MQTT broker.

## Quick start

```bash
python3 install.py     # bootstraps PlatformIO (macOS/Pi aware), verifies the build
./flash.sh             # build + upload (picks the serial port)
./monitor.sh           # serial monitor
```

1. On first boot the node opens WiFi AP **`vu2cpl-esp32-winkeyer-setup`** (password `vu2cpl1234`).
   Join it, pick your network, enter its password. Creds persist in NVS.
2. Copy `include/secrets.h.example` → `include/secrets.h` and set the MQTT role
   password (from the shack password manager). `secrets.h` is git-ignored.

## MQTT

- Broker: `192.168.1.10:1883` (auth required — role account in `secrets.h`).
- Status: `shack/esp32-winkeyer/status` (retained; LWT publishes `{"event":"offline"}`).

## Status

- ✅ Keyer core: iambic A/B (Curtis semantics), sidetone, PTT sequencing,
  speed pot, paddle break-in, tune mode — 1 kHz task on core 1, timing
  independent of WiFi. Serial test CLI on 115200 (`/status` for help).
- ⏳ WK3 protocol engine, WiFi TCP bridge (`winkeyer.local`) — next up.

Pin map (paddle tip=dit GPIO25, ring=dah GPIO26, key GPIO33, PTT GPIO32,
sidetone GPIO4, pot GPIO34): see `include/pins.h` and `HANDOVER.md`.

## Layout

```
platformio.ini        env:esp32-winkeyer (esp32dev) + env:esp32s3-winkeyer (esp32-s3-devkitc-1)
include/config.h       broker, topics, AP name  (+ git-ignored secrets.h)
include/pins.h         GPIO map (OTRSP pins reserved)
include/keyer.h        keyer core API
src/keyer.cpp          iambic keyer engine (1 kHz task, core 1)
src/main.cpp           WiFi (non-blocking portal) + MQTT + serial test CLI
flash.sh / monitor.sh  serial-port pickers (never pin the port)
install.py             toolchain bootstrap, macOS/Pi branch
```

See `HANDOVER.md` for the full picture.
