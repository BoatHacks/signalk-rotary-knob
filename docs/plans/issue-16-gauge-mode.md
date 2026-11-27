# Plan: #16 Gauge mode

## Overview
A GAUGE mode that shows SignalK values as gauges, one per page, so the knob is also an instrument when nothing is being
controlled. Read only: nothing leaves the knob. The arc and the number are coloured by the path's `meta.zones` from the server
(owner, 2026-11-27: "make sure the gauges use zone metadata for coloring").

The owner asked for it to be built without a mockup round, on a sane specification (2026-11-27). The decisions below are that
specification; each is easy to change.

## Relevant SPEC / ARCHITECTURE sections
SPEC.md section 6.3 (the paths and meta the knob reads), section 9 (configuration). docs/ui-design.md sections 2 (mode ring),
3.5 (MODES page), 3.8 (the screen), 7 (LEDs).

## Decisions
- **Pages:** up to four, set on the settings page as `gauge1_path` .. `gauge4_path`. Defaults: `environment.depth.belowTransducer`,
  `navigation.speedOverGround`, `environment.wind.speedApparent`, `electrical.batteries.house.voltage`. An empty path removes
  the page. `gaugeN_name` overrides the name. Paths apply after a restart, like the light channels.
- **Everything else from the path's meta** (it comes with the stream, sendMeta): `units`, `displayScale`, `zones`, `timeout`,
  `shortName`, `displayName`. No per-page unit, range or zone settings on the knob: zones are set once on the server and
  every display agrees.
- **Units:** fixed marine display units, not the server's unit preferences: m/s as knots, K as °C, ratio as %, rad as degrees,
  Pa as hPa, Hz as rpm, C as Ah, J as Wh, m3 as litres. Anything else is shown in its SI unit. Unit preferences can come later.
- **Range of the arc:** `meta.displayScale`, else a default for the unit and kind of path (depth 0..30 m, wind 0..40 kn, boat
  speed 0..12 kn, a 12, 24 or 48 V battery by its voltage, % 0..100, °C -10..40 or 0..120 for engines, hPa 960..1050, rpm
  0..4000), else around the zones' bounds (half their span on each side), else from the values seen. The ones from zones or
  values grow to hold every value seen since boot, so the needle never sticks at an end it does not belong to.
- **Zones:** a thin band outside the arc, green for normal and nominal, amber for alert and warn, red for alarm and
  emergency. The fill, the number and the zone's message (under the unit) take the colour of the zone the value is in. On a
  shared bound the more severe zone wins. Up to six zones per path.
- **Name:** `gaugeN_name`, else `meta.shortName`, else a short name from a table of common paths (DEPTH, SOG, AWS,
  HOUSE BATTERY, MAIN RPM, FRESHWATER ...), else `meta.displayName`, else the last part of the path. Long names (over ten
  characters) in 14 px.
- **Turn:** next or previous page, wrapping. Press and long press do nothing. Double press opens the ring as everywhere.
- **Stale:** `meta.timeout`, else 30 s. A stale or missing value shows `---` and `NO DATA`, no fill. Display only: no stale
  overlay for one gauge, the link overlay as in every mode.
- **No alarms of its own:** a zone in alarm colours the gauge, it does not take over the screen. Alarms from zones belong to
  the server (its notifications), not to a display.
- **LEDs:** the four rim LEDs show where the value is on the arc (`knob_led_fill`), blue while normal, amber in a warning
  zone, red in an alarm zone. Off at night, like the other levels.
- **Ring and MODES page:** GAUGE after MUSIC on the ring (eight slots with a USB host, the slot boxes shrink from 62 to 58 px),
  gauge icon (Font Awesome tachometer). The MODES page gets a seventh pill: rows of two, three and two. On by default.
- **Demo mode:** four simulated pages with zones (depth with shallow zones, SOG, wind with reef and gale zones, a 12 V
  battery), whatever paths are set.

## Approach
The rules are pure C in `components/knob_logic/knob_gauge.c` (host tested): the meta scanner (top level keys only, so a
`zones` inside another object is not taken), the unit table, the range, the zone lookup and the view the screen draws. The
model keeps one `knob_gauge_src_t` per page, written by the stream task only and copied out under the spinlock, and puts a
`knob_gauge_view_t` per page in the snapshot. The UI draws fractions of the arc and never sees SI units.

## Test strategy
- Host tests in `test/host`: zones parsing and lookup, meta, units, ranges, names, formatting, the view, the demo.
- `tools/ui-render`: the gauge pages in the manual's pictures (normal, warning, alarm, night).
- Hardware checklist: the live pages against the boat's server, zones from a path that has them, the LEDs, night.

## Open questions
- The server's unit preferences instead of the fixed table.
- More than one value per page, or a compass or wind rose page.

## Steps
- [x] knob_gauge in knob_logic, host tests
- [x] Config keys, model, snapshot, LEDs
- [x] Screen, ring slot and icon, MODES page
- [x] Renderer pictures, docs
- [ ] Hardware checks (docs/hardware-checklist.md)

## Files
`components/knob_logic/knob_gauge.[ch]`, `knob_logic.[ch]`, `components/knob_model/knob_model.c`,
`components/knob_config/knob_config.[ch]`, `config/knob.json`, `components/marine_ui/marine_ui.c`, `include/marine_ui.h`,
`fonts/knob_icon_28.c`, `components/knob_power/knob_power.c`, `tools/gen_fonts.sh`, `tools/ui-render/ui_render.c`,
`test/host/*`, docs.
