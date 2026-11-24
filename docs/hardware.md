# Hardware notes: Elecrow CrowPanel 1.28" Rotary Display

Research notes for the firmware spec. Pin values marked (src) were checked against Elecrow's own example code in
[Elecrow-RD/CrowPanel-1.28inch-HMI-ESP32-Rotary-Display-240-240-IPS-Round-Touch-Knob-Screen](https://github.com/Elecrow-RD/CrowPanel-1.28inch-HMI-ESP32-Rotary-Display-240-240-IPS-Round-Touch-Knob-Screen)
(`example/Arduino/Examples/Lesson03-Rotate`, `example/Arduino/RotaryScreen_1_28`). Everything else comes from the
Elecrow wiki, CNX Software, It's FOSS and the RIOT OS board page, and is unverified on hardware.

## Specs

| Item | Value |
|---|---|
| SoC | ESP32-S3R8, dual core LX7 @ 240 MHz, 512 KB SRAM |
| PSRAM | 8 MB octal (RIOT page says 2 MB, treat as wrong) |
| Flash | 16 MB |
| Display | 1.28" round IPS, 240x240, GC9A01 over SPI |
| Touch | CST816D, I2C addr 0x15 |
| Input | Rotary encoder (endless, no mechanical end stops) plus push |
| LEDs | 5x WS2812 ("ambient"), power LED, red LED |
| Wireless | Wi-Fi b/g/n 2.4 GHz, BLE 5 |
| Power | 5 V / 1 A. No battery connector found in any source |
| USB | Not a USB-C socket on the board. Native USB D+/D- sit on the 12 pin FPC, delivered via a MX1.25 to USB cable (50 cm, included) |
| Connectors | 12 pin FPC (UART, I2C, USB, IO4, IO12), 2x 4 pin MX1.25 UART, 1x 4 pin MX1.25 I2C |
| Buttons | BOOT, RESET (bottom), knob press |
| Size / weight | 48 x 48 x 33 mm, 50 g, aluminium + plastic + acrylic, M2.5 mounting |
| Temp | -20 to 65 C operating |
| Price | about 29 USD (Elecrow) |

## Pin map

| Function | GPIO |
|---|---|
| Encoder A / B (src) | 45 / 42 |
| Encoder push (src) | 41 (active low, INPUT_PULLUP) |
| Display SCLK / MOSI / DC / CS / RST (src) | 10 / 11 / 3 / 9 / 14 |
| Backlight PWM (src) | 46 |
| Touch SDA / SCL / INT / RST (src) | 6 / 7 / 5 / 13 |
| NeoPixel data, 5 LEDs (src) | 48 |
| Power LED (src) | 40 (driven low at boot) |
| Power rail enables (src) | 1 and 2, both must be HIGH (RIOT: 1 gates LCD+touch, 2 gates 5 V on the MX1.25 connectors) |
| UART0 / MX1.25 UART | TX 43 / RX 44 |
| I2C1 (FPC / MX1.25 I2C) | SDA 38 / SCL 39 |
| Free on FPC | 4, 12 (ADC capable) |
| BOOT | 0 |

Encoder A/B use no internal pull-ups in the Elecrow code and are decoded by polling a quadrature state table
(4 transitions per step). The firmware should use the ESP32-S3 PCNT peripheral or a GPIO interrupt instead.

### Ambient LED positions

The five WS2812s sit evenly spaced (72 degrees apart) around the rim of the base. Measured with `spikes/led-id`,
looking at the knob from the front, the data line runs counter-clockwise:

| Data-line index | Clock position |
|---|---|
| 0 | about 3:30 |
| 1 | about 1 o'clock |
| 2 | about 11 o'clock |
| 3 | about 8:30 |
| 4 | 6 o'clock |

All five positions were confirmed on the bench.
A clockwise sweep therefore walks the indexes downwards: 4, 3, 2, 1, 0.

### Encoder

Measured 2026-11-24 with `spikes/encoder-timing` (PCNT x4 as in the firmware, sampled every 1 ms):

- **30 detents per full turn, 4 edges per detent.** Every rest during slow turning was exactly 4 edges after the last, as
  `BOARD_ENC_EDGES_PER_STEP` assumes. Turning right counts up.
- **Time between clicks:**

| Turning | Median | Range |
|---|---|---|
| Very slow, one click at a time (full turn) | 918 ms | 769 to 987 ms |
| Slow, as for fine adjustment | 288 ms | 251 to 332 ms |
| Normal | 75 ms | 58 to 156 ms |
| Fast spin | 20 ms | 10 to 93 ms (90 % under 93 ms) |

- During the fast spin 12 of 50 rests sat between two detents (the hand holding the knob mid-detent while regripping, or
  edges lost at speed; not settled). Dividing by 4 makes this harmless.
- Used by the motor mode's step presets (#8): fast under 50 ms, slow over 150 ms, normal between.

## Facts that shape the firmware

- Display and touch power is switched by GPIO1 and must be enabled before init. External 5 V on the MX1.25 sockets
  is switched by GPIO2.
- A SignalK knob can be fed over Wi-Fi (WebSocket / REST to the SignalK server) or over USB if TinyUSB mode is used
  (Elecrow lesson 6 does a HID mouse, needs `ARDUINO_USB_MODE=0`, which costs the USB serial console).
- Elecrow Arduino setup: esp32 core 3.3.8, board "ESP32S3 Dev Module", 16 MB flash, OPI PSRAM, custom partition
  `elecrow_s3` (app 10 MB OTA ota_0 only, 896 KB SPIFFS, no second OTA slot).
  Their examples use LVGL 9.1.0 + LovyanGFX + Adafruit NeoPixel.
- ESP-IDF, PlatformIO, ESPHome and MicroPython are all possible. RIOT has no quadrature decoder for ESP32 yet.
- Hardware reports: slight wobble around the knob edge, device sits raised on the cable side, MX1.25 cable is hard
  to remove, no haptics, no battery, no sensors on board.
- Unknown until measured: encoder pulses per revolution, detent feel, button travel, backlight and LED current
  draw (the 1 A budget matters on a boat 12 V to 5 V supply), IP rating (none claimed, so the enclosure must be
  splash proofed by us).

## Open questions to verify on the bench

1. Do 4, 12, 38, 39 reach the FPC as documented? (The wiki is inconsistent: OLED vs DHT20 on 38/39.)
2. Is native USB (GPIO19/20) really exposed on the FPC, and can the knob run as a USB HID device and power itself from one port?
3. Encoder PPR and whether it needs hardware debounce.
4. Idle current with the display dimmed and with Wi-Fi on.

## Sources

- [Elecrow wiki](https://elecrow.com/wiki/CrowPanel_1.28inch-HMI_ESP32_Rotary_Display.html)
- [Elecrow Arduino course index](https://www.elecrow.com/wiki/1.28_inch_Rotary_Display_Arduino_Course_Index.html)
- [Elecrow GitHub repo (examples, Eagle schematic PDF)](https://github.com/Elecrow-RD/CrowPanel-1.28inch-HMI-ESP32-Rotary-Display-240-240-IPS-Round-Touch-Knob-Screen)
- [CNX Software](https://www.cnx-software.com/2025/09/19/elecrow-esp32-s3-rotary-displays-combine-round-ips-touchscreen-knob-and-press-input/)
- [It's FOSS review](https://itsfoss.com/crowpanel-rotary-display-review/)
- [RIOT OS board page](https://doc.riot-os.org/boards/elecrow-crowpanel-esp32s3-rotary-128)
