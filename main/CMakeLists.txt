idf_component_register(SRCS "main.c" "ubx.c" "gnss_web.c" "ble_link.c" "web_codec.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_uart esp_driver_i2c esp_timer bt nvs_flash)
