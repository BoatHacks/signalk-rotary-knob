# Plan: first release (CI, signed OTA, espOS registry, cleanup)

## Overview
Make the knob releasable the way signalk-espOS-8relay is: every push built and tested in GitHub Actions, releases built,
signed and attached by a workflow, boards updated over the air from a manifest, and the firmware listed in the espOS
registry so the signalk-espos-manager plugin and the web flasher can install it. Before that, clean the repo up for a
public audience.

## Relevant SPEC / ARCHITECTURE sections
ARCHITECTURE.md sections on espOS, OTA and the build. SPEC section 12 (security). hardening.md (USB PID, still open).

## Reference: how 8relay does it
- `build.yml`: on push to main and on pull requests, an ESP-IDF container builds the firmware with a throwaway signing key
  and runs the host tests.
- `cut-release.yml`: started by hand (Actions, "Cut release", version `vX.Y.Z`, pre-release ticked by default). It checks
  that CHANGELOG.md has a dated entry and a one-line summary, tags the head of main, then calls `release-firmware.yml`.
- `release-firmware.yml`: restores the real key from the secret `SIGNING_KEY_PEM` and refuses to build a release without it
  (unless `ALLOW_UNSIGNED_RELEASE` is set, which never reaches the manifest). It builds, merges a full-flash image, attaches
  `-merged.bin`, `-ota.bin` and the `.elf` to the release, mirrors the images to a `release-assets` branch (GitHub release
  downloads have no CORS header, raw.githubusercontent does), and adds the build to `manifest.json` on an `ota` branch:
  full releases on `stable` and `beta`, pre-releases on `beta`.
- Firmware side: `main.c` writes the manifest URL
  (`https://raw.githubusercontent.com/<repo>/ota/manifest.json`) into espOS's `ota.manifest_url` once, on the first boot.
- Registry: `projects/8relay.json` in signalk-espOS/registry with `app` (the CMake `project()` name), `signingKeyId`,
  boards, channels, asset patterns and `webAssetsBranch`.

Since then espOS publishes reusable workflows (`build-firmware.yml`, `release-firmware.yml`, docs/releasing.md in espOS).
They do the build, merge, per-flash-size images and the mirror, so a project keeps about fifteen lines of workflow. They
expect the ESP-IDF project at the repository root (`.idf-version` and `idf.py build` there), take tags as plain `vX.Y.Z`
(a beta is a tag released as a GitHub pre-release), and name files `<name>-<board>-<tag>-merged.bin` and `-ota.bin`. They
do not write an OTA manifest, so that job stays ours (8relay's `update_ota_manifest.py` and its tests).

## Approach
1. **Decisions first** (see Open questions). The app name, the key and the layout cannot change after the first board is
   in the field without stranding it.
2. **Cleanup** so the repo reads as a project, not a bench log (list below).
3. **Layout**: move `firmware/` to the repository root, like 8relay, so espOS's reusable workflows work unchanged.
   `test/`, `tools/`, `docs/`, `spikes/` and `ui/` stay where they are.
4. **CI**: `build.yml` calls espOS's `build-firmware.yml` (throwaway key) and runs `test/host` with make.
5. **Releases**: a hand-started `cut-release.yml` as in 8relay (it fits the rule that releases are cut only when asked;
   release-please would open release PRs on every push), which tags and then calls espOS's `release-firmware.yml`,
   pinned to an espOS release tag, with `signing_key: ${{ secrets.SIGNING_KEY_PEM }}`. Our own `manifest` job follows it.
6. **OTA**: `main.c` sets the default manifest URL once (8relay's `default_ota_manifest`). Before the image is marked valid,
   a failed display, touch or encoder init marks it invalid, so a broken update rolls back (8relay review item 6.1).
7. **Registry**: a board file for the Elecrow CrowPanel 1.28" rotary display (not in the catalogue yet) and
   `projects/rotary-knob.json`, as a pull request titled `add: rotary-knob`, after the first signed release exists.

### Cleanup list
- `firmware/CMakeLists.txt`: the header is copied from espOS's registry example (third-party copyright line and text about
  "this repository"). Replace it with our own.
- `partitions.csv`: the comment talks about the Waveshare P4 panels. Say what the table is for here.
- `main/idf_component.yml`: drop the "Spike" header, pin espOS to one exact version (8relay does), keep IDF `>=6.1,<6.2`.
- `main.c` and component headers: drop "bring-up stage", add SPDX lines where missing.
- README: what it is, a photo, install with the manager or the web flasher, build from source, status. Move bench notes out.
- New: CHANGELOG.md (Keep a Changelog, with the one-line summary the manifest uses), CONTRIBUTING.md, `scripts/` from 8relay
  (`changelog_section.sh`, `changelog_summary.sh`, `update_ota_manifest.py` and its tests).
- docs: retire `docs/HANDOFF.md` and `docs/spike-espos.md` into ARCHITECTURE.md or `docs/history/`; delete finished plans
  (internal-ram-dip, boot-to-last-screen once its checks pass), as the plans README says.
- `ui/marine_ui_v5/`: keep as the design record or move under `docs/design/`.
- `tools/build.ps1`: keep as the local helper, document it in CONTRIBUTING.
- Third-party: credit LVGL, Montserrat, esp_lcd_gc9a01 and led_strip in a NOTICE file. Check that `assets/splash.png` is ours.
- Issues #2 to #7: built, waiting for their hardware checklist sections. Close each one when its section passes.
- Before the repo goes public: one more scan of the whole history for addresses, tokens, SSIDs and MAC addresses (a first
  scan on 2026-11-24 found none of the known ones).

## Test strategy
- CI: the firmware build and `test/host` on every push and pull request. Manifest script tests (`test/scripts`) from 8relay.
- A first pre-release `v0.1.0` cut through the workflow, then on the bench: USB flash of `-merged.bin` on a blank board,
  OTA from the manifest to a `v0.1.1` pre-release on the beta channel, a wrongly signed image refused, and a forced rollback
  (an image that fails its init check). Add these as a section of `docs/hardware-checklist.md`.
- Registry: `node scripts/validate.mjs` and `reindex.mjs` in a registry checkout before the pull request.

## Open questions
- **App name** (owner). The registry and the OTA manifest match on the CMake `project()` name, today `knob`. That is too
  generic to identify the firmware. Proposal: `signalk-rotary-knob`. The bench knob then needs one USB flash.
- **Signing key** (owner). Proposal: a new production key, `espsecure generate-signing-key --version 2 --scheme rsa3072`,
  made by the owner, stored as the secret `SIGNING_KEY_PEM` plus an offline backup. Its id goes into the registry entry as
  `signingKeyId`. The bench knob, signed with the local dev key, moves to it with one USB flash.
- **Repository visibility** (owner). The `ota` and `release-assets` branches must be public to be read by boards and the
  flasher. The repo is private today.
- **USB PID** (owner, from hardening.md). A released firmware that shows up as a USB device should not keep a borrowed PID.
- **First version**: `v0.1.0` as a pre-release, `v1.0.0` after the live control trials and a pre-1.0 review like 8relay's
  plan 22?
- **Registry status**: listed as a community project with `support: tested` on the CrowPanel 1.28.

## Steps
- [x] Decided 2026-11-24 (owner: "let's go" on the proposals): app name `signalk-rotary-knob`, first release `v0.1.0` as a
      pre-release. Still with the owner: the key, the repo visibility, the USB PID.
- [x] Cleanup: CMakeLists header and NOTICE, partitions comment, exact espOS pin, main.c header, the dummy
      `environment.inside.temperature` publish removed (a spike leftover that wrote a fake value to the boat), README,
      CONTRIBUTING, CHANGELOG, manual section 7, bench-era docs to `docs/history/`.
- [x] Move `firmware/` to the root, fix `tools/build.ps1`, the docs and the spike's relative paths. Build locally.
- [x] `build.yml` with espOS's `build-firmware.yml` (v0.32.0) plus host tests. Green on GitHub since 2026-11-24.
- [x] CMake project renamed, default manifest URL written once on the first boot. No extra rollback check: a failed hard
      init aborts and the bootloader rolls the unconfirmed image back, and rolling back on a soft fault (no touch answer) would
      make a knob with a broken touch panel refuse every update.
- [x] CHANGELOG.md, `scripts/`, `test/scripts` (10 tests pass), `cut-release.yml` with the manifest job, hardware
      checklist section "Release and OTA".
- [x] Production key created 2026-11-24 (key id `4e0a983dc96231e6`), stored as the secret `SIGNING_KEY_PEM`. Backup: owner.
- [x] 2026-11-25: whole-history scan (addresses, SSIDs, MACs, tokens, keys, host and user names): no hits. The commits carry the
      author's `dev@` address and two early messages carry session links, both become public with the repo (owner to accept).
- [x] 2026-11-25: `cut-release.yml` checks `version.txt` against the version (the image reports that file, not the tag). README
      status corrected, it claimed bench tests that have not been done. Changelog brought up to date.
- [ ] Owner: origin and licence of `assets/splash.png` (nothing records it), then NOTICE.
- [ ] Repo public (owner), branch protection with the oss-branch-protection template.
- [ ] Cut `v0.1.0` as a pre-release (owner says when). Bench checks: USB flash, OTA, bad signature, rollback.
- [ ] Registry: board file and project entry, validate, pull request (owner approves posting).
- [ ] Pre-1.0 review, live control trials, then `v1.0.0`.

## Files
`firmware/**` (moved to the root), `.github/workflows/build.yml`, `.github/workflows/cut-release.yml`, `CHANGELOG.md`,
`CONTRIBUTING.md`, `NOTICE`, `README.md`, `scripts/*`, `test/scripts/*`, `main/main.c`, `main/idf_component.yml`,
`partitions.csv`, `CMakeLists.txt`, `tools/build.ps1`, `docs/hardware-checklist.md`, `docs/plans/README.md`. In
signalk-espOS/registry: `boards/elecrow-crowpanel-1.28-rotary.json`, `projects/rotary-knob.json`.
