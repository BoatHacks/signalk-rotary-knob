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

## Results so far (2026-11-22, espOS 0.32.0, board on COM9)

| # | Question | Result |
|---|---|---|
| 1 | Builds for esp32s3? | Yes, but needs **IDF 6.1.x**. The `from_registry` example pins `>=6.1.0,<6.2.0`. Built with 6.1.1. 6.0.3 and the cockpit firmware's pin belong to older espOS. |
| 2 | Boots, portal, Wi-Fi, token? | Boots and opens the setup portal `espOS-adc4` (open AP, 192.168.4.1). Wi-Fi join, SignalK discovery and token still untested: needs Wi-Fi credentials entered by hand. |
| 6 | Timing | `app_main` at 329 ms, portal up at 489 ms after reset (no display yet). Wi-Fi join and `SK_STREAM_CONNECTED` not measured yet. |
| 7 | Memory | Boot log shows about 251 KiB + 21 KiB + 32 KiB internal heap regions. Healthy for 40 s, no health alarms. PSRAM not enabled in the spike, so PSRAM headroom is untested. |
| 9 | Flash layout | espOS's 16 MB table works as is: two 6.5 MB app slots, 1.75 MB LittleFS storage. App is 1.38 MB (79% of a slot free). |

Not tested yet: 3 (display before network), 4 (GC9A01 + CST816D), 5 (encoder indev, dimmer wake), 8 (USB composite).

Findings that affect the spec:
- Build needs the espOS signing key (`espsecure generate-signing-key`, kept out of git) even with OTA unused.
- With the stock `sdkconfig.defaults`, SNTP init fails (`CONFIG_LWIP_DHCP_GET_NTP_SRV` is off). Harmless, but needs a config line.
- The REST API is open on the network until `httpd.api_key` is set, and the setup AP is open.
- espOS 0.32.0 started cleanly on the S3 at default 80 MHz DIO flash, with OTA validated on boot (`running knob_spike 0.1.0 in ota_0 (valid)`).
- Reading flash with the esptool stub failed at one page (0x1e6000) on this unit. Writing was fine.

## Outcome

Write down for each question: works, works with changes, or blocked, and any workaround. Then pick:
espOS as the base, espOS components selectively, or plain ESP-IDF.
