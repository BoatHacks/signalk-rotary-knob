# Handoff: signalk-rotary-knob

State of the project on 2026-11-23, written so a new session or a Claude project can pick it up cold. It deliberately
contains no machine paths, device identifiers, addresses, credentials or personal details, because the repo may become
public. Anything of that kind lives outside the repo.

## What this is

Firmware for the Elecrow CrowPanel 1.28" rotary display (ESP32-S3, round 240x240 touch screen, endless encoder with push
button, 5 RGB LEDs) as a helm-side SignalK control surface: autopilot, lights (relays or dimmers) and anchor alarm. The knob
talks to the SignalK server directly, there is no companion plugin in the MVP. Built on espOS (ESP-IDF 6.1, LVGL 9.3).
Part of the BoatHacks SignalK suite.

## Read these first

| File | What it holds |
|---|---|
| [SPEC.md](../SPEC.md) | The spec: purpose, rules, state model, API, MVP, design decisions (section 12), open questions and the boat survey (section 13) |
| [ARCHITECTURE.md](../ARCHITECTURE.md) | Components, tasks, data flow, security, deployment, the espOS gate table and the implementation order (section 9) |
| [docs/ui-design.md](ui-design.md) | The agreed UI: input vocabulary, modes, alarm and stale rules, power and LEDs, style tokens |
| [docs/hardware.md](hardware.md) | Board specs and the verified pin map |
| [docs/manual.md](manual.md) | User manual, draft 1: first setup, controls, modes, alarms, settings, troubleshooting |
| [docs/hardware-checklist.md](hardware-checklist.md) | What has been checked on the panel and the boat, and what is still open |
| [docs/plans/](plans/README.md) | Plans for the open work and issues, in the suggested order |
| [IMPLEMENTATION_CHECKLIST.md](../IMPLEMENTATION_CHECKLIST.md) | How a change is made in this repo, from reading to commit |
| [docs/spike-espos.md](spike-espos.md) | The espOS feasibility spike: results and the decision (espOS as base, gated) |
| `firmware/` | The firmware: `main/` plus components (board, lv_port, input, knob_net, knob_model, knob_actions, knob_logic, knob_config, knob_power, knob_usb, marine_ui) |
| `ui/marine_ui_v5/prototype/` | Browser prototype of the UI, the design reference |
| `tools/` | `build.ps1` (build and flash with regular status lines), `png2lvgl.py` (splash image converter) |

## Where it stands

- **Hardware works** with the espOS base: display, encoder, touch (with a centre dead zone and ghost filter), LEDs, idle dim.
  USB is the chip's USB-Serial/JTAG port (logs, bench commands, flashing). Only while the USB-HID mode is on screen does
  TinyUSB take the port over as a HID-only device (switching not yet tried with a host).
- **marine_ui runs on the panel** from a snapshot: boot splash with a live progress bar and phase text, autopilot, lights,
  anchor, mode ring, alarm screens, stale overlay, pairing and setup screens, steering LEDs, night palette.
- **Wi-Fi and SignalK work against the real boat server.** The knob joins Wi-Fi, sends its access request, gets a readwrite
  token, opens the delta stream and survives reboots and reflashes. Online about 5 s after boot (target 10 s). An
  authenticated `PUT` to a relay was verified.
- **knob_model, knob_config and the adapters run on the board.** Real subscriptions, the lights adapter (Relay 1 verified end
  to end), the autopilot v2 and hoekens-anchor-alarm adapters behind a live-control switch that is off at every boot (serial
  command `l`), settings from the espOS settings store (`config/knob.json`). Autopilot and anchor have not sent a real command
  yet. What has been checked is in [hardware-checklist.md](hardware-checklist.md), open work in [plans/](plans/README.md).

## Decisions worth knowing (full list in SPEC.md section 12)

- Controls autopilot (SignalK v2 API, provider id from settings), lights and the anchor alarm (hoekens-anchor-alarm REST, circle
  zones only). The boat has relays and no dimmers, so lights are on/off channels (turn right is on, left is off) and dimmers
  stay supported.
- One credential: the SignalK access-request token. Plain WebSocket and HTTP on the boat LAN, no TLS.
- Engage the autopilot with a long press plus a confirm turn. Anchor changes need only a press. Touch never changes autopilot
  or anchor state.
- Firmware updates over USB in the MVP, OTA and a signed firmware repository later.
- Staleness is measured from the knob's receive time, per value.
- Autopilot and anchor commands must be deliberately enabled: the stored setting allow control, off by default (SPEC section 2).

## Next steps

1. Compile and fix `knob_model` and `knob_config`, flash, check the lights slice end to end with the knob's own relay channel.
2. Try the autopilot adapter against the real pilot with the owner present (live control on), then the anchor adapter with a real
   drop and raise. Verify that the drag alarm acknowledges through the Notifications API.
3. The stream drop after boot is worked around with `time.sk_fallback` off. The real fix (espOS sending per-path unsubscribes that
   signalk-server rejects) is to be investigated later.
4. Hardening before any unattended use: the setup access point closes once Wi-Fi works, the API key is optional (decided
   2026-11-23, the instrument Wi-Fi is trusted), check that the
   portal can take the key with the Wi-Fi credentials.
5. USB-HID dial mode, config export and import (espOS already exports its config as JSON), the open issues for the manual, the
   REST API, demo mode and an on-device settings screen.

## Open questions

The live list is SPEC.md section 13. Notably: the real alarm acknowledge route, GPS-lost alarm source, the heading reference label
(`°T` or `°M`) which needs a snapshot field, which relays beyond the first become channels, and the USB VID/PID before the repo
is shared.

## Gotchas learned

- espOS is pre-1.0 and single-maintainer. Pin versions and keep `dependencies.lock`.
- signalk-server only accepts "unsubscribe all" on the stream and closes it on a per-path unsubscribe. espOS sends one when its
  SignalK fallback clock is no longer needed, so set `time.sk_fallback` off on every knob. The knob must not call
  `espos_sk_unsubscribe` itself either.
- The setup portal is an **open** access point and the REST API has no key until `httpd.api_key` is set. The key is optional by
  decision, but the access point comes back after 90 s without Wi-Fi, so set `wifi.portal_psk` on a knob that may run without
  its network in a crowded place.
- The build requires an app signing key (`espsecure generate-signing-key --version 2 --scheme rsa3072`). Keep it out of git
  (`*.pem` is ignored). A lost key means a USB reflash.
- **Reflashing:** the knob's USB port is the chip's USB-Serial/JTAG port, so `idf.py flash` resets into the bootloader by
  itself. Not while the USB-HID mode is on screen: TinyUSB owns the port then, leave the mode first or use BOOT plus RESET
  (release BOOT afterwards). Firmware from before 2026-11-23 used a TinyUSB CDC port and the 1200 baud reboot, which leaves the
  chip's force-download flag set: after flashing from that state, tap RESET (or send an esptool watchdog reset).
- **FreeRTOS tick is 10 ms.** `pdMS_TO_TICKS(5)` is 0 and `vTaskDelay(0)` does not sleep. A task that does that starves
  everything of lower priority.
- **Internal RAM is the scarce resource.** espOS and Wi-Fi take about 125 KB, espOS's health watchdog restarts after three
  strikes of less than 12 KB free or a largest block under 8 KB. Kept in check by: LVGL on a PSRAM allocator, all of the knob's
  own task stacks except LVGL's in PSRAM (`xTaskCreateWithCaps`), Wi-Fi and LWIP buffers allowed in PSRAM
  (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`). Result on 2026-11-23: about 60 KB free, low-water mark 39.8 KB. The boot log prints
  the free RAM after each init step at 20 s and 60 s. The espOS publish buffer grows while no server is reachable.
- The knob's own `POST /api/v1/system/reboot` needs a JSON content type, otherwise it answers 415.
- Windows Defender Firewall blocks inbound connections to programs without a rule. A relay for the knob (home LAN to the boat
  over WireGuard) worked as an `ssh -L` forward but not as a Python script.
- Disk: ESP-IDF, tools and a build are several GB. Keep the build directory outside the source tree (`idf.py -B`).
- GPIO1 and GPIO2 must be driven high before the LCD and touch controller work.
- Elecrow's wiki disagrees with itself in places. The pin map in hardware.md was checked against Elecrow's example code.

## Setting this up as a Claude project

Suggested knowledge: SPEC.md, ARCHITECTURE.md, docs/ui-design.md, docs/hardware.md and this file. Suggested standing
instructions:

- Separate commits from releases. Do not cut a release or bump the version unless asked.
- Do not build or flash after every change. Make the change, report, and wait to be told.
- Do not run the full test suite after every change.
- Ask before each push.
- Never commit signing keys, Wi-Fi credentials, tokens or device identifiers. Put anything of that kind outside the repo.
- Treat claims about SignalK APIs and espOS internals as unverified until checked against the source or a live server.
  SPEC.md marks these with **(verify)**.
- Log time for each finished task in the shared time tracking file of the BoatHacks laserbrain repo.
