idf_component_register(SRCS "knob_model.c"
                       INCLUDE_DIRS "."
                       REQUIRES lvgl input knob_hid knob_actions knob_logic board lv_port marine_ui knob_config knob_net knob_power knob_usb espos_core espos_sk espos_wifi esp_app_format esp_timer)
