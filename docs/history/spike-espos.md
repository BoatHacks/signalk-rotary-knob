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

## Results so far (2026-11-22, espOS 0.32.0, one CrowPanel 1.28" board)

| # | Question | Result |
|---|---|---|
| 1 | Builds for esp32s3? | Yes, but needs **IDF 6.1.x**. The `from_registry` example pins `>=6.1.0,<6.2.0`. Built with 6.1.1. 6.0.3 and the cockpit firmware's pin belong to older espOS. |
| 2 | Boots, portal, Wi-Fi, token? | Boots and opens the setup portal `espOS-xxxx` (open AP, 192.168.4.1). Wi-Fi join, SignalK discovery and token still untested: needs Wi-Fi credentials entered by hand. |
| 6 | Timing | `app_main` at 329 ms, portal up at 489 ms after reset (no display yet). Wi-Fi join and `SK_STREAM_CONNECTED` not measured yet. |
| 7 | Memory | Boot log shows about 251 KiB + 21 KiB + 32 KiB internal heap regions. Healthy for 40 s, no health alarms. PSRAM not enabled in the spike, so PSRAM headroom is untested. |
| 9 | Flash layout | espOS's 16 MB table works as is: two 6.5 MB app slots, 1.75 MB LittleFS storage. App is 1.38 MB (79% of a slot free). |

Second round (raw hardware, no LVGL yet, code in `spike/main/board.c`):

| # | Question | Result |
|---|---|---|
| 3 | Display before network? | Yes. Power rails, GC9A01 and the test pattern run before `espos_start()`. Boot log shows the first frame about 0.6 s after reset, espOS up shortly after. Not yet measured to a stable UI. |
| 4 | GC9A01 + CST816D | GC9A01 initialises over SPI at 40 MHz (`espressif/esp_lcd_gc9a01` 2.0.4, BGR order, inverted, as in Elecrow's example). CST816D ACKs at 0x15 after a reset pulse on GPIO13. **Visual check of colours/orientation and touch coordinates still needs a person at the device.** |
| 5 | Encoder | PCNT x4 unit and button input are created without errors. **Turn direction, edges per detent and button polarity still need a person at the device.** Not yet an LVGL indev, dimmer wake untested. |

The test pattern should show: top band red, right green, bottom blue, left yellow, black centre, a white square orbiting
the centre as the dial turns (24 steps per lap, red while pressed) and a centre box that turns red on press. If colours
look swapped or inverted, flip `rgb_ele_order` or the invert call in `board_display_init()`.

Third round (LVGL 9.3 port, PSRAM, on the device with a person present):

| # | Question | Result |
|---|---|---|
| 5 | Encoder as LVGL indev, dimmer wake | Turn direction and one value step per click were confirmed by a person. Idle dim after 10 s works. A push reaches the widget (`button down`/`button up` in the log) only after a fix: the encoder read callback must set `data->key = LV_KEY_ENTER`, otherwise LVGL never sends PRESSED/CLICKED to the focused widget. The push fix was seen in the log, not yet confirmed on screen. **Wake from dim by a turn or push is not yet confirmed**: the "return from idle" seen earlier may have been a ghost touch (see below). |
| 4 | Display orientation | Needed a left-right mirror (`esp_lcd_panel_mirror(panel, true, false)`). Colours and the red-top/green-right/blue-bottom/yellow-left orientation are right after that. |
| 4 | Touch | Mapping already matches the mirrored picture with no flip: centre about (120,118), top edge y=13, right edge x=230. |
| 7 | Memory | **Without PSRAM, LVGL plus espOS boot-looped** (`espos_sk_start(): ESP_ERR_NO_MEM`, internal RAM exhausted by LVGL's 64 KB pool and two 19 KB DMA draw buffers). Fixed with 8 MB octal PSRAM at 80 MHz, LVGL on `malloc`, allocations over 4 KB in PSRAM. Boots stably, health watchdog normal. |
| 6 | Timing | PSRAM memory test costs about 0.43 s of boot (`CONFIG_SPIRAM_MEMTEST=n` would save it). LVGL screen is up about 0.37 s after `app_main`. |

Hardware findings:
- **A knob push reads as a touch.** It presses the glass: about (138,111), about 230 ms before the button switch closes and
  about 70 ms after it opens. Handled for now with a 50 px dead zone around the centre. Masking touches around pushes is on the
  later list (SPEC.md post-MVP).
- **The CST816D reports ghost touches** such as `x=64 y=3840` about every 16 s while the backlight is dimmed. They are far out of
  range and used to wake the dimmer, so the driver now drops out-of-range reports. Whether auto-sleep or calibration causes
  them is not investigated.
- A touch that lands on a clickable widget behaves like a press on it. Make widgets that only the encoder should drive
  non-clickable (the encoder path does not check the flag).

Fourth round (USB composite, esp_tinyusb 2.4.0, `spike/main/usb_dev.c`):

| # | Question | Result |
|---|---|---|
| 8 | CDC + HID enumerate on the FPC/MX1.25 USB cable? | **Yes.** Windows shows one composite device with a CDC serial port and a HID consumer-control device. The display, encoder, dimmer and espOS run together with it. The HID descriptor is only a consumer-control placeholder: no HID report has been sent to the host yet, and the "dial" profile is not built. |
| 8 | Does espOS stay happy? | Not confirmed. The app ran for minutes without a reboot, but no espOS log lines were seen after USB came up and the setup access point was not visible in the test PC's Wi-Fi scan. The Wi-Fi step has to settle this. |

Findings that affect the spec and architecture:
- **TinyUSB takes the native USB pins from USB-Serial/JTAG.** The USB-Serial/JTAG console, the ROM log and the auto-reset into download mode disappear while it runs. Logs have to go to the CDC port (`tinyusb_console_init`), and boot messages before the host opens the port are lost. Logs only flow while a host has the port open with DTR set.
- **Reflashing needs a way back into the ROM bootloader.** The spike reboots into download mode when the CDC port is opened at 1200 baud (sets the force-download bit, then restarts). Verified: the board reappeared as a ROM device, esptool talked to it, and it returned to the app on reset. The BOOT button is the fallback. The product firmware needs the same trick or a reachable BOOT button.
- The USB identity changes the COM port (USB-Serial/JTAG, composite and ROM download are three different devices). Tooling must not assume one port.
- The spike uses the Espressif VID with a throwaway PID. A shipped knob needs its own VID/PID.
- Image is now 1.77 MB, about 73% of an app slot still free.

Still not tested: Wi-Fi join and SignalK token, boot time to a connected stream, sending HID reports.

Findings that affect the spec:
- Build needs the espOS signing key (`espsecure generate-signing-key`, kept out of git) even with OTA unused.
- With the stock `sdkconfig.defaults`, SNTP init fails (`CONFIG_LWIP_DHCP_GET_NTP_SRV` is off). Harmless, but needs a config line.
- The REST API is open on the network until `httpd.api_key` is set, and the setup AP is open.
- espOS 0.32.0 started cleanly on the S3 at default 80 MHz DIO flash, with OTA validated on boot (`running knob_spike 0.1.0 in ota_0 (valid)`).
- Reading flash with the esptool stub failed at one page (0x1e6000) on this unit. Writing was fine.

## Outcome

**Decision (2026-11-22): espOS as the base, gated.** Works: 1 (IDF 6.1.x), 3, 4, 5 (turn, one step per click, idle dim; push and
wake still to confirm at the board), 7 (needs PSRAM), 8 (enumerates, espOS alive unconfirmed), 9. Works with changes: 4 (mirror, ghost
touch filter, centre dead zone), 5 (encoder key), 8 (console moves to CDC, 1200 baud reflash path). Not yet answered: 2 and 6 (Wi-Fi
join, token, boot to stream). The gate for keeping espOS is in ARCHITECTURE.md section 9. If it fails, fall back to espOS components
selectively, then plain ESP-IDF.
