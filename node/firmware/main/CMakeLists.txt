idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES pm_protocol pm_artnet pm_rdm pm_board pm_port pm_net pm_config pm_ota pm_ui
                                nvs_flash esp_timer)
