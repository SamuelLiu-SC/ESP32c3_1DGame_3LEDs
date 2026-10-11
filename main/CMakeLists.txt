idf_component_register(SRCS "blink_example_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_driver_gpio esp_driver_i2c nvs_flash)
