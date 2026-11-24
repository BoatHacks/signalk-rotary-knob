/* SPDX-License-Identifier: Apache-2.0
 * The knob's USB port: the USB-Serial/JTAG console normally, a HID-only device (through esp_tinyusb) while the USB-HID mode
 * is active. */
#pragma once

#include <stdbool.h>

void usb_dev_init(void); /* installs the USB-Serial/JTAG driver (console and bench commands) and the switch task */

/* Switch the port to a HID device (true) or back to the serial port (false). Returns at once, the switch runs on its own task.
 * Called with the active mode, so the knob is a HID device only while the USB-HID mode is on screen. */
void usb_dev_set_hid(bool on);

/* A USB host is on the cable: the serial port sees it, or, while HID is on, the host has enumerated the HID device. Decides
 * whether the USB-HID mode is offered on the mode ring. */
bool usb_dev_host_present(void);

/* The HID device is enumerated by a host and reports can be sent. */
bool usb_dev_mounted(void);

/* Report ids of the HID interface, used by knob_hid. */
#define USB_HID_RID_KEYBOARD 1
#define USB_HID_RID_MOUSE    2
#define USB_HID_RID_CONSUMER 3

/* Called from the USB task for every byte received on the serial port (bench commands). */
typedef void (*usb_dev_rx_cb_t)(char c);
void usb_dev_set_rx_cb(usb_dev_rx_cb_t cb);
