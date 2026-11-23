/* SPDX-License-Identifier: Apache-2.0
 * USB-HID dial mode: turns the knob's dial and push into HID reports for a host, by profile.
 *
 *   HID_PROFILE_CHARTPLOTTER  mouse wheel (zoom) and left button, for a chart program such as OpenCPN
 *   HID_PROFILE_MEDIA_AUDIO   volume up and down, push is mute
 *   HID_PROFILE_NAV_KEYS      left and right arrow keys, push is Enter
 *
 * Reports are queued and sent by a small task when the endpoint is free, so the callers never block. Nothing is sent while no
 * host has enumerated the knob. */
#pragma once

#include "marine_ui.h"

void knob_hid_start(void);
void knob_hid_turn(int delta);      /* signed steps, clockwise positive */
void knob_hid_press(void);
void knob_hid_next_profile(void);
usb_hid_profile_t knob_hid_profile(void);
