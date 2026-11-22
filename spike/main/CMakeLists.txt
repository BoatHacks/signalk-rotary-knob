idf_component_register(SRCS "main.c" "board.c" "lv_port.c" "app_ui.c"
                       INCLUDE_DIRS "."
                       REQUIRES espos_core espos_sk esp_lcd esp_driver_spi esp_driver_gpio esp_driver_ledc
                                esp_driver_pcnt esp_driver_i2c esp_timer lvgl__lvgl)
