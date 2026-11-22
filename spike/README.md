# knob_spike

Throwaway bring-up project for the Elecrow CrowPanel 1.28" rotary display (ESP32-S3), used to test whether
[espOS](https://github.com/signalk-espOS/espOS) is a sound base for the firmware. Nothing here ships. See
[../docs/spike-espos.md](../docs/spike-espos.md) for the questions and results, and
[../docs/HANDOFF.md](../docs/HANDOFF.md) for the project state.

What it does: starts the display and inputs first (`main/board.c`), then `espos_start()`, then publishes a dummy
SignalK value once a second.

## Provenance

`CMakeLists.txt`, `sdkconfig.defaults`, `partitions.csv`, `.idf-version` and `version.txt` are adapted from espOS's
`from_registry` example (Apache-2.0, copyright Dirk Wahrheit, SPDX headers kept). `main/main.c`, `main/board.c` and
`main/board.h` are new.

## Build and flash

Needs ESP-IDF **6.1.x** (espOS 0.32 requires `>=6.1.0,<6.2.0`), target `esp32s3`, 16 MB flash.

```sh
# 1. an app signing key (espOS signs every image; keep it out of git, *.pem is ignored)
espsecure generate-signing-key --version 2 --scheme rsa3072 secure_boot_signing_key.pem

# 2. configure, build, flash (use -B to keep the build directory outside the source tree)
idf.py -B <build-dir> set-target esp32s3
idf.py -B <build-dir> build
idf.py -B <build-dir> -p <serial-port> flash
```

The board's console is the native USB-Serial/JTAG (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`).

## Notes

- `dependencies.lock` pins the component versions that were tested.
- The espOS setup portal is an **open** Wi-Fi access point and the REST API has no key until `httpd.api_key` is set.
  Do not leave a spike board running unattended.
