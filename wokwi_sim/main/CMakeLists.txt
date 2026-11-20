idf_component_register(
    SRCS "main.c" "../../common/circular_buffer.c"
    INCLUDE_DIRS "." "../include"
    REQUIRES esp_driver_gpio esp_driver_uart esp_driver_i2c esp_timer esp_system
)
