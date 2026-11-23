idf_component_register(SRCS "knob_actions.c"
                       INCLUDE_DIRS "."
                       REQUIRES knob_hid knob_config knob_net knob_power marine_ui espos_core espos_sk espressif__cjson esp_timer)
