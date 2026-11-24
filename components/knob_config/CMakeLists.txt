idf_component_register(SRCS "knob_config.c"
                       INCLUDE_DIRS "."
                       REQUIRES espos_config nvs_flash)
# The descriptor adds the "knob" namespace to the espOS settings page and the REST config API.
espos_config_add_descriptor(config/knob.json)
