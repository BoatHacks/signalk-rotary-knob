# Plans

Working plans for open work, one file per issue or topic. A plan is short: what and why, which SPEC and ARCHITECTURE sections it
implements, the approach, how it is checked, the steps and the files. Requirements belong in SPEC.md, not here. Delete a plan
when the work is done and the docs say everything it said. How a change is made: [IMPLEMENTATION_CHECKLIST.md](../../IMPLEMENTATION_CHECKLIST.md).

Name files `issue-<n>-<short-name>.md` for a GitHub issue, `<short-name>.md` otherwise. Start from [_template.md](_template.md).

## Open work, in the suggested order

| Plan | Issue | Why this order |
|---|---|---|
| [internal-ram-dip.md](internal-ram-dip.md) | none | A boot with 276 bytes of internal RAM left can reboot the knob. Blocks everything else |
| [live-control-trials.md](live-control-trials.md) | none | Autopilot and anchor have never run against the real boat. Needs the owner at the boat |
| [boot-to-last-screen.md](boot-to-last-screen.md) | none | Gate item not met, small, needs one decision |
| [hardening.md](hardening.md) | none | Required before the knob runs unattended or the repo goes public |
| [issue-3-rest-api.md](issue-3-rest-api.md) | #3 | The settings screen and demo mode both want the same config interface |
| [issue-5-settings-screen.md](issue-5-settings-screen.md) | #5 | Design agreed. Builds on #3's config reload and the allow control key |
| [issue-4-demo-mode.md](issue-4-demo-mode.md) | #4 | Useful for the manual's screenshots |
| [issue-6-volume-mode.md](issue-6-volume-mode.md) | #6 | A new mode on signalk-jukebox, survey and design done |
| [issue-7-autopilot-modes.md](issue-7-autopilot-modes.md) | #7 | Needs the SeaTalk pilot wired to the server, compass mode proven on the boat, and an upstream fix for wind targets |
| [issue-2-user-manual.md](issue-2-user-manual.md) | #2 | Follows the behaviour, so it finishes last |

The plans for the open issues (#2 to #7) were agreed with the owner on 2026-11-23, with mockups for #5, #6 and #7. Their
decisions are in SPEC.md section 12 and docs/ui-design.md sections 3.1, 3.5 and 3.6. None of them has open questions left,
only facts marked **(verify)** that the work checks first.

## Decisions waiting for the owner

- Which relays beyond Relay 1 become light channels, and their names (SPEC section 13).
- The boot screen rule (see boot-to-last-screen.md).

Decided 2026-11-23 while planning the issues: "live control" becomes the stored setting **allow control** (SPEC section 2),
switched on the web page, the config API or the knob's settings screen.
