/* Spike: USB composite device (CDC log console + HID consumer control) through esp_tinyusb. Throwaway code. */
#pragma once

#include <stdbool.h>

void usb_dev_init(void); /* installs TinyUSB, CDC and HID, routes stdout/logs to the CDC port */
bool usb_dev_mounted(void);
