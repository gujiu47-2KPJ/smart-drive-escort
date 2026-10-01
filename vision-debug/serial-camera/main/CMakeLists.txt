idf_component_register(SRCS "serial_camera_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp32-camera esp_driver_uart freertos)
