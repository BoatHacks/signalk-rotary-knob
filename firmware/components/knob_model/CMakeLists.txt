idf_component_register(SRCS "knob_model.c"
                       INCLUDE_DIRS "."
                       REQUIRES board lv_port marine_ui knob_config knob_net knob_power knob_usb espos_core espos_sk espressif__cjson esp_timer)
