idf_component_register(SRCS "src/pm_ota.c" "src/pm_ota_session.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       REQUIRES pm_protocol esp_common
                       PRIV_REQUIRES app_update esp_partition esp_timer esp_system freertos log)
