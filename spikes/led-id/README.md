# LED identification spike

Minimal firmware that lights the five ambient WS2812s in a fixed pattern, so their positions on the ring can be
mapped. Data-line index 0 is white, 1 green, 2 blue, 3 red, 4 off. Nothing else runs (no display, no Wi-Fi, no espOS).

It uses the knob firmware's partition table and app signing key, so it flashes over the knob firmware without
touching NVS (settings, Wi-Fi, SignalK token), and the knob firmware flashes back over it the same way.

Result: see "Ambient LED positions" in [docs/hardware.md](../../docs/hardware.md).

Build and flash with ESP-IDF 6.1:

```
idf.py -B <build dir> build
idf.py -B <build dir> -p <port> flash
```
