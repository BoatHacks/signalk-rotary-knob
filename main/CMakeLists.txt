idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES board lv_port input knob_api knob_model knob_net knob_power knob_usb marine_ui espos_core espos_config espos_sk esp_timer nvs_flash)
