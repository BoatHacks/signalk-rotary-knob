# Spike: espOS on the CrowPanel 1.28" (ESP32-S3)

Goal: decide whether espOS is a sound base before we write ARCHITECTURE.md. Time box: one day. Throwaway code,
nothing from the spike ships.

## Questions to answer

1. Does `espos_core` build for `esp32s3` with the IDF version it pins? The registry says `>=6.0.0,<6.1.0`, the espOS
   README says 6.1, the cockpit firmware uses `v6.0.3` (`.idf-version`). Which one works?
2. Does the espOS `minimal` example boot on the board, open the `espOS-xxxx` setup portal, join Wi-Fi and get a SignalK
   token (access request approved on a real server)?
3. Can we start the display before the network, as the cockpit firmware does with the `before_network` hook?
4. Does the GC9A01 (SPI) plus CST816D (I2C, addr 0x15) come up under IDF 6, with GPIO1 and GPIO2 driven HIGH first?
5. Does the encoder (GPIO45/42, push on 41) work through PCNT or a GPIO interrupt, as an LVGL encoder indev with a
   `lv_group`, and does it reset LVGL's inactivity counter so an idle dimmer can wake on it?
6. Timing: power-on to first frame, to Wi-Fi up, to `SK_STREAM_CONNECTED` (targets: 4 s and 10 s).
7. Memory: free internal RAM and PSRAM with espOS, LVGL and one screen running (S3 has 8 MB PSRAM, 512 KB SRAM).
8. USB: does esp_tinyusb composite (CDC + HID) enumerate on the FPC/MX1.25 USB cable, and does espOS stay happy?
9. Flash layout: which partition table (single OTA slot vs two), and what espOS needs (LittleFS for the web UI).

## Steps

1. Install the pinned ESP-IDF, then `idf.py add-dependency "signalk-espos/espos_core"`, start from the `minimal` example,
   target `esp32s3`, 16 MB flash, octal PSRAM.
2. Add a small board component modelled on `cockpit_hal` (`DisplayDriver`, `TouchDriver`): GC9A01 over SPI at pins
   from [hardware.md](hardware.md), backlight PWM on GPIO46, power rails on GPIO1/2.
3. LVGL 9.3 with one arc screen, encoder indev plus group, touch indev.
4. Wire the display bring-up into the espOS before-network hook, log timestamps at each espOS event
   (`NETWORK_UP`, `SK_TOKEN_APPROVED`, `SK_STREAM_CONNECTED`).
5. Subscribe to `environment.mode` and `navigation.headingTrue` via `espos_sk_subscribe`, PUT to one dimmer path via
   `espos_sk_put`.
6. Composite USB device, then check enumeration on a PC while Wi-Fi stays connected.
7. Record numbers for questions 6 and 7.

## Outcome

Write down for each question: works, works with changes, or blocked, and any workaround. Then pick:
espOS as the base, espOS components selectively, or plain ESP-IDF.
