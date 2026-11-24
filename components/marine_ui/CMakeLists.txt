idf_component_register(SRCS "marine_ui.c" "splash_img.c"
                            "fonts/knob_font_14.c" "fonts/knob_font_20.c" "fonts/knob_font_32.c"
                       INCLUDE_DIRS "include"
                       REQUIRES lvgl esp_timer knob_logic)
