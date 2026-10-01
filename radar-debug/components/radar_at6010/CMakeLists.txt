idf_component_register(
    SRCS "radar_at6010.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart esp_driver_gpio esp_timer
)
