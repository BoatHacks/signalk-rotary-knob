# Handoff: signalk-rotary-knob

State of the project at the end of the first working session (2026-11-22), written so a new session or a Claude
project can pick it up cold. It deliberately contains no machine paths, device identifiers, credentials or personal
details, because the repo may become public. Anything of that kind lives outside the repo.

## What this is

Firmware for the Elecrow CrowPanel 1.28" rotary display (ESP32-S3, round 240x240 touch screen, endless encoder with
push button, 5 RGB LEDs) as a helm-side SignalK control surface: autopilot, dimmer and anchor alarm. A companion
SignalK server plugin owns pairing, layouts and path mapping. Part of the BoatHacks SignalK plugin suite.

## Read these first

| File | What it holds |
|---|---|
| [SPEC.md](../SPEC.md) | Draft 1 of the spec: purpose, rules, state model, API sketch, MVP, design decisions, open questions (section 13) |
| [docs/hardware.md](hardware.md) | Board specs and the verified pin map, plus bench questions |
| [docs/spike-espos.md](spike-espos.md) | The espOS feasibility spike: questions, steps and results so far |
| `spike/` | The spike project (throwaway): espOS 0.32.0 plus raw display/encoder/touch bring-up |

`ARCHITECTURE.md` has **not** been written on purpose. It waits for the spike outcome.

## Decisions made (all recorded in SPEC.md section 12)

- SignalK control surface for autopilot, dimmer and anchor alarm. Autopilot via the SignalK v2 autopilot API, so any
  autopilot with a provider plugin works.
- Wi-Fi through a companion plugin, plus USB-HID as a selectable mode (one transport per mode, Wi-Fi stays up in HID mode).
  HID profile is configurable, default a HID dial.
- Plain WebSocket on the boat LAN, authenticated by a pairing token. No TLS.
- Always powered, so a dim mode is needed. Dim after idle, never fully off while the autopilot is engaged or an alarm is
  active, and follow `environment.mode` (`"day"`/`"night"`). Default to Day until the first value.
- Boot targets: last screen within 4 s, ONLINE within 10 s.
- Engage the autopilot with a long press plus a confirm turn. Anchor changes need only a press to confirm.
- First Wi-Fi credentials through a captive portal.
- Firmware is **not** pulled from the companion plugin. Flashing is over USB.
- ESP-IDF, not Arduino. Build on espOS with our own knob app, borrowing ideas and code (Apache-2.0, 2.x line only) from
  `dirkwa/espos-p4-cockpit` (display/touch HAL split, idle dimmer, day/night state). The cockpit's touch-first JSON Layout
  Player is out of the MVP.
- The companion plugin stays as specced even though espOS's own SignalK client could talk to the server directly.

## Where the spike stands

Build and runtime on the real board, with espOS 0.32.0:

- Needs **ESP-IDF 6.1.x**. 6.0.x does not satisfy the component constraint.
- Builds for `esp32s3` with espOS's 16 MB partition table (two 6.5 MB app slots, 1.75 MB web UI storage). The app is about
  1.4 MB.
- Boots, opens the espOS setup portal, validates the OTA image, runs its health watchdog, no crashes in a 40 s capture.
- Display: the GC9A01 comes up over SPI (40 MHz, BGR, inverted). Touch: the CST816D answers at I2C 0x15. Encoder: PCNT
  x4 unit and push button initialise cleanly. First frame about 0.6 s after reset.
- Display before network works (`espos_start()` runs after the panel is up).

**Not verified by a person at the device yet:** that the test pattern shows the right colours and orientation, which way
the encoder counts, edges per detent, button polarity, and touch coordinates. The expected behaviour is described in
the spike notes.

**Not tested at all:** LVGL on this panel, the encoder as an LVGL input device with a focus group, dimmer wake from the
encoder, USB composite (CDC + HID) alongside Wi-Fi, Wi-Fi join and SignalK access request, PSRAM headroom, boot time to
SignalK stream connected.

## Next steps, in order

1. Have someone at the device confirm the test pattern and encoder behaviour, fix `rgb_ele_order`/invert if colours are off.
2. Add LVGL 9.3 on the panel. Encoder as an LVGL indev with a group, touch as a pointer indev. Check the encoder resets
   LVGL's inactivity counter so a dimmer can wake on it.
3. Join Wi-Fi through the portal, approve the device in the SignalK server's access requests, subscribe to
   `environment.mode` and PUT one dimmer path. Measure boot to `SK_STREAM_CONNECTED`.
4. Composite USB (esp_tinyusb): CDC for logs plus HID, with Wi-Fi up.
5. Enable PSRAM and measure headroom with LVGL plus espOS.
6. Write up the spike outcome, then decide: espOS as base, espOS components selectively, or plain ESP-IDF. Only then write
   ARCHITECTURE.md.

## Open questions

The live list is SPEC.md section 13. At time of writing: night/day publisher on the boat, dimmer path convention, which
anchor alarm plugin to drive (espOS docs target `hoeken/hoekens-anchor-alarm`, early research used sbender9's
`anchoralarm`), what to do about espOS's signed OTA (left on by default, nothing points it at an update source),
overlap between espOS's access-request pairing and our plugin's pairing, whether simple paths use espOS's SignalK client
directly, whether the encoder can wake the dimmer.

Also undecided: the repo has no licence file yet. The spike reuses Apache-2.0 files from espOS and keeps their headers.

## Gotchas learned

- espOS is pre-1.0 and single-maintainer, and the registry constraint on ESP-IDF changed between releases. Pin versions
  and keep `dependencies.lock`.
- The setup portal is an **open** access point and the REST API has no key until `httpd.api_key` is set. Do not leave a
  spike board running unattended.
- The build requires an app signing key (`espsecure generate-signing-key --version 2 --scheme rsa3072`). Keep it out of
  git (`*.pem` is ignored). Images signed with one key are rejected by a device flashed with another, so a lost key means a
  USB reflash.
- The native USB-Serial/JTAG is the board's console and flashing path. With the stock firmware, esptool's stub loader
  failed to read one flash page on the unit tested while the ROM loader read it fine, so back up a board with a fallback
  to `--no-stub`.
- Disk: ESP-IDF plus tools plus a build is several GB. Keep the build directory outside the source tree (`idf.py -B`).
- GPIO1 and GPIO2 must be driven high before the LCD and touch controller work.
- Elecrow's wiki disagrees with itself in places (GPIO 38/39 use, PSRAM size on a third-party page). The pin map in
  hardware.md was checked against Elecrow's example code, not just the wiki.

## Setting this up as a Claude project

Suggested knowledge: SPEC.md, docs/hardware.md, docs/spike-espos.md and this file. Suggested standing instructions:

- Separate commits from releases. Do not cut a release or bump the version unless asked.
- Do not run the full test suite after every change. Update tests as you go and run them for a release or on request.
- Ask before each push.
- Never commit signing keys, Wi-Fi credentials, tokens or device identifiers. Put anything of that kind outside the repo.
- Treat claims about SignalK APIs and espOS internals as unverified until checked against the source or a live server.
  SPEC.md marks these with **(verify)**.
