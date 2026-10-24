idf_component_register(SRCS "src/pm_config.c"
                       INCLUDE_DIRS "include"
                       REQUIRES pm_protocol esp_common
                       PRIV_REQUIRES nvs_flash)
