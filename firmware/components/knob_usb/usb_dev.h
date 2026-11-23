/* SPDX-License-Identifier: Apache-2.0
 * USB composite device (CDC log console + HID consumer control) through esp_tinyusb. */
#pragma once

#include <stdbool.h>

void usb_dev_init(void); /* installs TinyUSB, CDC and HID, routes stdout/logs to the CDC port */
bool usb_dev_mounted(void);

/* Report ids of the HID interface, used by knob_hid. */
#define USB_HID_RID_KEYBOARD 1
#define USB_HID_RID_MOUSE    2
#define USB_HID_RID_CONSUMER 3

/* Optional: called from the USB task for every byte received on the CDC port (bench commands during bring-up). */
typedef void (*usb_dev_rx_cb_t)(char c);
void usb_dev_set_rx_cb(usb_dev_rx_cb_t cb);
