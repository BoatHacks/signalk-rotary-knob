# Contributing

Issues and pull requests are welcome. For anything bigger than a fix, open an issue first so the design can be agreed:
requirements live in [SPEC.md](SPEC.md), the code layout in [ARCHITECTURE.md](ARCHITECTURE.md), the screens in
[docs/ui-design.md](docs/ui-design.md), and how a change is made in [IMPLEMENTATION_CHECKLIST.md](IMPLEMENTATION_CHECKLIST.md).

## Building

Needs ESP-IDF 6.1.x (`.idf-version`), target `esp32s3`. The repository root is the ESP-IDF project.

```sh
# Once: an app signing key. Every image is signed and a knob only accepts updates signed with the key it was first
# flashed with. *.pem is ignored by git, never commit it.
espsecure generate-signing-key --version 2 --scheme rsa3072 secure_boot_signing_key.pem

idf.py -B <build dir> set-target esp32s3
idf.py -B <build dir> build
idf.py -B <build dir> -p <serial port> flash monitor
```

A knob flashed with your own key will not take the project's release updates over the air (and the reverse). Flash a
release's `-merged.bin` over USB to go back.

The console is the chip's USB-Serial/JTAG port, with automatic reset for flashing. In the USB-HID mode the port is a HID
device instead: leave that mode, or press RESET, to flash.

On Windows, `tools/build.ps1` builds and optionally flashes with progress lines and a short boot log afterwards
(`Get-Help tools/build.ps1`).

## Tests

- Host tests of the pure logic (`components/knob_logic`): `make -C test/host test UNITY_DIR=$IDF_PATH/components/unity/unity/src`.
- Release script tests: `python3 -m unittest discover -s test/scripts`.
- On the device: [docs/hardware-checklist.md](docs/hardware-checklist.md).

CI runs the firmware build and both test sets on every push and pull request (`.github/workflows/build.yml`).

## Releasing (maintainers)

1. Set `version.txt` to the version (`0.2.0`): it is what the knob reports, and the workflow refuses a mismatch. Under
   `## [Unreleased]` in [CHANGELOG.md](CHANGELOG.md), rename the heading to the version and date
   (`## [0.2.0] - 2026-12-01`) and check its first line: one summary of at most 127 bytes, which knobs show as the
   update's notes. Commit and push to main.
2. Actions, **Cut release**, Run workflow on main with the version (`v0.2.0`). "Publish as a pre-release" is ticked by
   default: pre-releases reach knobs on the beta channel only, a full release reaches stable and beta.
3. The workflow tags main, creates the release, builds and signs the firmware with the repository secret
   `SIGNING_KEY_PEM`, attaches the images, mirrors them for the web flasher (`release-assets` branch) and adds the build to
   the OTA manifest (`ota` branch). Re-run it with the same version to retry a failed run.

Versions are plain `vX.Y.Z`. Before 1.0.0, any release may change behaviour or settings.
