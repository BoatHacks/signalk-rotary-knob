idf_component_register(SRCS "knob_hid.c"
                       INCLUDE_DIRS "."
                       REQUIRES knob_usb marine_ui espressif__esp_tinyusb esp_timer)
