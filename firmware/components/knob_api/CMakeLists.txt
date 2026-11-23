idf_component_register(SRCS "knob_api.c"
                       INCLUDE_DIRS "."
                       REQUIRES knob_actions knob_logic knob_model knob_usb marine_ui espos_httpd esp_http_server esp_app_format)
