# pid.codes request (draft, not sent)

Not needed for now: the knob uses Espressif's ready-made 303A:4004 (2026-11-25).

The two files for a pull request to [pidcodes/pidcodes.github.com](https://github.com/pidcodes/pidcodes.github.com), at the same
paths there (`org/BoatHacks/index.md`, `1209/B0A7/index.md`). 0xB0A7 was free on main and in every open pull request on
2026-11-25, check again before sending. Rules: https://pid.codes/howto/.

Pull request title: `Add BoatHacks and PID 0xB0A7 for the SignalK rotary knob`

Text:

> Hi, I'd like to request a PID for my rotary knob firmware.
>
> It's open source firmware (Apache-2.0) for an off-the-shelf ESP32-S3 knob with a round display, the Elecrow CrowPanel 1.28". It
> turns it into a control knob for boats running SignalK. In USB mode it acts as a HID device for a chartplotter or PC. It's
> firmware only, I don't have any hardware design of my own in this project.
>
> Why a PID for the firmware: the firmware makes it a specific device with its own HID reports, and I'd like it to show up as the
> same device on everybody's boat instead of using a generic ID. Users just install a release, so it doesn't make sense for each
> of them to request their own.
>
> Source: https://github.com/BoatHacks/signalk-rotary-knob

Once merged: VID 0x1209 and PID 0xB0A7 in `components/knob_usb/usb_dev.c`, and the notes in ARCHITECTURE.md and
docs/plans/hardening.md.
