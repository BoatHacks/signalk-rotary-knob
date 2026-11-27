# Plans

Working plans for open work, one file per issue or topic. A plan is short: what and why, which SPEC and ARCHITECTURE sections it
implements, the approach, how it is checked, the steps and the files. Requirements belong in SPEC.md, not here. Delete a plan
when the work is done and the docs say everything it said. How a change is made: [IMPLEMENTATION_CHECKLIST.md](../../IMPLEMENTATION_CHECKLIST.md).

Name files `issue-<n>-<short-name>.md` for a GitHub issue, `<short-name>.md` otherwise. Start from [_template.md](_template.md).

## Open work, in the suggested order

| Plan | Issue | Why this order |
|---|---|---|
| [live-control-trials.md](live-control-trials.md) | #12 | Autopilot and anchor have never run against the real boat. Needs the owner at the boat |
| [boot-to-last-screen.md](boot-to-last-screen.md) | none | Built and checked 2026-11-23. Left: anchor and USB-HID restore checks |
| [hardening.md](hardening.md) | none | Only the setup without an API key is left to try |
| [issue-7-autopilot-modes.md](issue-7-autopilot-modes.md) | #7 | Built. Needs the SeaTalk pilot wired to the server, compass mode proven on the boat (#12), and an upstream fix for wind targets |
| [issue-8-motor-mode.md](issue-8-motor-mode.md) | #8 | Built, most of it checked in demo mode. The motor side (plugin and path) is not decided, so the knob is built against configurable defaults and the demo |
| [issue-10-auto-demo.md](issue-10-auto-demo.md) | #10 | Built. Left: the internal RAM dip, the fonts of zone and channel names, the bench checks, a video |
| [issue-16-gauge-mode.md](issue-16-gauge-mode.md) | #16 | Built 2026-11-27. Left: the hardware checks |
| [issue-2-user-manual.md](issue-2-user-manual.md) | #2 | Follows the behaviour, so it finishes last |
| [release.md](release.md) | #13 | v0.1.0 and v0.1.1 are out. Left: the rest of the release and OTA checks, the espOS registry, v1.0.0 |

## Built, only checks left

| Plan | Issue | State |
|---|---|---|
| [internal-ram-dip.md](internal-ram-dip.md) | none | Fixed 2026-11-23 (39.8 KB minimum at boot). Left: five cold boots. The automatic demo dips lower, see #10 |
| [issue-3-rest-api.md](issue-3-rest-api.md) | #3 | Built, issue closed 2026-11-25. Its checks are in #11 |
| [issue-4-demo-mode.md](issue-4-demo-mode.md) | #4 | Built, issue closed 2026-11-25. Its checks are in #11 |
| [issue-5-settings-screen.md](issue-5-settings-screen.md) | #5 | Built, issue closed 2026-11-25. Its checks are in #11 |
| [issue-6-volume-mode.md](issue-6-volume-mode.md) | #6 | Built, issue closed 2026-11-25. Its checks are in #11 |
| [issue-9-led-levels.md](issue-9-led-levels.md) | #9 | Built, issue closed 2026-11-25. Its checks are in #11 |

The plans for #2 to #7 were agreed with the owner on 2026-11-23, with mockups for #5, #6 and #7. Their
decisions are in SPEC.md section 12 and docs/ui-design.md sections 3.1, 3.5 and 3.6. None of them has open questions left,
only facts marked **(verify)** that the work checks first.

## Decisions waiting for the owner

- Which relays beyond Relay 1 become light channels, and their names (SPEC section 13).

Decided 2026-11-23 while planning the issues: "live control" becomes the stored setting **allow control** (SPEC section 2),
switched on the web page, the config API or the knob's settings screen.
