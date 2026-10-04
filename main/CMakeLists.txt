idf_component_register(SRCS "bt_app_core.c"
                            "main.c"
                    PRIV_REQUIRES bt nvs_flash esp_driver_i2s esp_driver_gpio esp_timer
                    INCLUDE_DIRS ".")
