idf_component_register(SRCS "usb_dev.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_timer espressif__esp_tinyusb)
