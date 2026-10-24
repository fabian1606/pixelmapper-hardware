idf_component_register(SRCS "src/pm_config.c" "src/pm_config_store.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       REQUIRES pm_protocol pm_board esp_common
                       PRIV_REQUIRES nvs_flash esp_timer esp_driver_gpio freertos log)
