idf_component_register(SRCS "src/pm_ota.c"
                       INCLUDE_DIRS "include"
                       REQUIRES pm_protocol esp_common
                       PRIV_REQUIRES app_update esp_partition)
