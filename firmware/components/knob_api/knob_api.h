/* SPDX-License-Identifier: Apache-2.0
 * The knob's own REST endpoints on the espOS httpd (#3). The config itself is espOS's /api/v1/config?ns=knob.
 *
 *   GET  /api/v1/knob/mode     {"mode":"lights"}
 *   PUT  /api/v1/knob/mode     {"mode":"autopilot"|"lights"|"anchor"|"volume"|"hid"}, the same entry point as the mode ring.
 *                              It cannot engage, drop, raise or acknowledge anything. 400 for a bad body, 409 for HID without a
 *                              host or a mode switched off in the settings.
 *   GET  /api/v1/knob/status   {"mode","link","allow_control","demo","alarm","firmware"}, no paths, tokens or addresses
 *
 * All three are protected by the API key when one is set (espOS's default for registered handlers). */
#pragma once

/* Registers the endpoints. Call after espos_start() (the httpd runs then). */
void knob_api_start(void);
