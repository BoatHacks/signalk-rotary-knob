# Engineering Handoff: Elecrow 1.28" Marine UI (Package v5)

v5 = the v4 package with the v5 fix list applied (by the project owner's session, not the original UI agent). Everything not
listed here is unchanged from v4. **Not compiled and not run on hardware**: there was no compiler in the session. LVGL API
names and struct fields used by the new code were checked against the LVGL 9.3.0 headers and sources
(`lv_draw_*.h`, `lv_canvas.c`, `lv_anim.h`). The prototype (`prototype/index.html`) was run in a browser, no console errors,
and the stale logic was exercised from the console.

**Target:** ESP32-S3, 240 x 240 GC9A01, CST816D, ESP-IDF 6.1, LVGL 9.3, custom LVGL port.

## Changes since v4

| # | Item | Status | What changed |
| :-- | :-- | :-- | :-- |
| 1 | Curved text drew one glyph repeatedly | **Fixed** (verify on device) | `update_curved_canvas()` queued every glyph blit and finished the layer once, so all queued tasks read the scratch buffer after the last glyph was written. LVGL 9.3's `lv_canvas_finish_layer()` runs the queue synchronously (checked in `lv_canvas.c`), so each glyph now does `init_layer, lv_draw_image, finish_layer` before the scratch buffer is overwritten. `text_local = true` kept. |
| 2 | Stale only evaluated on snapshot | **Fixed** | `update_stale_overlay()` runs every `marine_ui_tick()` and from `marine_ui_apply_snapshot()`. It re-renders the active mode and the overlay only when the stale signature changes (and the age text once a second). |
| 3 | Stale model | **Fixed** | Every value has `<name>_updated_ms` and `<name>_timeout_ms` in the snapshot (0 = never stale by age, the firmware fills it from SignalK `meta.timeout` or a per-path default). New: `ap_state_*` and `dimmer_*` pairs. Display-only values (rudder, depth, voltage, GPS fix) mask only themselves (`---`). Essential values (AP state, heading, target; anchor data; dimmer levels) show the stale overlay with a reason (`NO AP STATE`, `NO HEADING`, `NO TARGET`, `NO ANCHOR DATA`, `NO LIGHT DATA`) and mask the numbers. Only a down stream shows `LINK LOST` with the age of the last data. `MARINE_STALE_VALUE_TIMEOUT_MS` is removed. |
| 4 | Stale trapped the user | **Fixed** | `dispatch_semantic()` no longer drops the double press and mode-ring input when stale. Only the mode's own controls are gated (after the ring and alarm handling). HID mode (`current_stale()`) ignores SignalK staleness. The stale overlay is hidden while the mode ring is open and while the full-screen alarm is up. Prompts and pending previews are cancelled when a mode goes stale or changes. |
| 5 | Glyphs missing from Montserrat | **Fixed** | Connecting steps are drawn shapes: green disc with `LV_SYMBOL_OK` (in the built-in symbol range) for done, cyan ring for the current step, grey ring for pending. No `✓●○` remain. The prototype draws the same shapes. |
| 6 | No icons in the C code | **Fixed** | `icon_draw()` draws AUTO, LIGHT, ANCHOR, USB and WARNING on a small ARGB8888 canvas with `lv_draw_line`, `lv_draw_arc`, `lv_draw_rect` (32 x 32 design grid, scaled). Used in the mode ring slots (54 x 48 box, 28 px icon above the label) and on the alarm (48 px anchor in the geofence, 22 px warning above the headline). Icons are redrawn only when their colour changes (night palette). The prototype draws the same shapes (`drawIcon`). |
| 7 | Tick ring cost | **Fixed** | The ring repaints only after >= 0.5 degrees of heading change and at most every 100 ms (`MARINE_TICK_MIN_DELTA_DEG`, `MARINE_TICK_MIN_INTERVAL_MS`). Colours are set only when the palette changes. The `pts` arrays are static. Frame time with the ring updating is **not measured** (needs hardware). |
| 8 | Alarm acknowledged state | **Fixed** | The acknowledged ring pulses slowly (triangle-wave opacity animation, only when `fx_ring_pulse_enabled`, steady otherwise). New action `MARINE_ACT_ALARM_LED_SLOW_PULSE` (1 on entering the acknowledged state, 0 on leaving). `MARINE_ACT_ALARM_LED_PULSE` now also fires with value 0 when the server clears the alarm. The backlight wakes and the LEDs flash on arrival **and** whenever the alarm type changes (`active_alarm != prev_alarm_state`). |

Also changed: `marine_ui_apply_snapshot()` keeps `now_ms` monotonic (a snapshot carrying an older `now_ms` than the tick no
longer moves the clock back). The alarm layout moved up to make room for the icons (geofence 84 px at y -34, headline at +40).

## Known limits and things to verify on hardware

1. Curved text: the per-glyph finish makes about 2 layer passes per character, only when the string changes. Measure the update time of the longest line (about 30 characters).
2. Frame time of the tick ring with heading updates, and of the glass overlays, with `fx_*` switches on and off.
3. Icon shapes were written blind (the arc angles for AUTO and ANCHOR follow LVGL's clockwise-from-3-o'clock convention). Look at them on the panel and adjust the 32 x 32 coordinates in `icon_draw()` if needed.
4. `lv_draw_arc_dsc_t.start_angle`/`end_angle` with `end < start` (AUTO gauge, 135 to 45): confirm the arc wraps. If it does not, draw it as two arcs.
5. Mode-ring selected box still uses a border and optional shadow, not the corner brackets of the prototype.
6. The firmware must pass the timeouts. Suggested defaults: AP state, heading, target, rudder 5 s; anchor and GPS 10 s; depth 10 s; voltage 30 s; dimmer 0.
7. `marine_ui_load_demo_snapshot()` leaves all `*_timeout_ms` at 0, so demo data never goes stale.

## Prototype

New buttons: **Stream Down (LINK LOST)**, **One Value Stale (depth)**, **Essential Value Stale** (freezes the active mode's heading, anchor or light data). A live-feed simulator keeps every value fresh except frozen ones (v4 let all values go stale after 10 s without input). The mode ring stays usable while a mode is stale.
