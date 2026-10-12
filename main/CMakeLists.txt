idf_component_register(SRCS "main.c" "ubx.c" INCLUDE_DIRS "."
                       REQUIRES esp_driver_uart esp_driver_i2c esp_timer)
