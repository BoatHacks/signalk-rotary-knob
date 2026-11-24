# Encoder timing spike

Minimal firmware for #8 (motor mode): measures how many detents one full turn of the knob has, how many quadrature edges
one detent is, and how far apart the clicks come when turning slowly, normally and fast. The motor mode's three step
presets (Gentle, Normal, Quick) take their click-time thresholds from this. Nothing else runs (no display, no Wi-Fi, no
espOS).

The encoder setup is the knob firmware's (`components/board/board.c`: PCNT, x4 quadrature, 1 us glitch filter), sampled
every 1 ms (1 kHz FreeRTOS tick here, the knob firmware runs at 100 Hz).

It uses the knob firmware's partition table and app signing key, so it flashes over the knob firmware without touching NVS
(settings, Wi-Fi, SignalK token), and the knob firmware flashes back over it the same way.

## Runs

Press the knob to start a run, turn, press again: the summary prints on the serial console.

1. **One full turn, slowly,** click by click, back to the start mark. The run's detents are the detents per turn. The rest
   lines show the edges per detent (`+4 edges since the last rest` for each click when it is 4).
2. **Slow:** turn as for fine adjustment at the dock, about ten clicks.
3. **Normal:** a relaxed turn, a few turns.
4. **Fast:** spin it quickly, a few turns.
5. Optionally the same to the left.

Each summary gives the net edges and detents, where the knob came to rest, and the time between clicks (min, p10, median,
p90, max and a histogram). Gaps over 1 s count as pauses and are left out.

Result: see "Encoder" in [docs/hardware.md](../../docs/hardware.md): 30 detents per turn, 4 edges per detent.

Known issue: in the 2026-11-24 runs only the first press registered, so no run ended and no summary printed. The numbers were
worked out from the click lines instead (split at pauses over 2 s). Not chased, the click log is enough.

Build and flash with ESP-IDF 6.1:

```
idf.py -B <build dir> build
idf.py -B <build dir> -p <port> flash
```
