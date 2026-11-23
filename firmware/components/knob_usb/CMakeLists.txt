idf_component_register(SRCS "usb_dev.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_timer espressif__esp_tinyusb esp_driver_usb_serial_jtag esp_hal_usb vfs)
