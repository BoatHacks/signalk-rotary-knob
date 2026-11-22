idf_component_register(SRCS "main.c" "demo_model.c"
                       INCLUDE_DIRS "."
                       REQUIRES board lv_port input knob_net knob_power knob_usb marine_ui espos_core espos_sk esp_timer)
