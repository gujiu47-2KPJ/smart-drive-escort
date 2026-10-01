idf_component_register(
    SRCS "imu_bmi270.c" "bmi270_config_file.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp_driver_gpio esp_timer
)
