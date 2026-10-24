idf_component_register(SRCS "src/pm_port.c"
                       INCLUDE_DIRS "include"
                       REQUIRES pm_board pm_rdm pm_protocol esp_common
                       PRIV_REQUIRES esp_driver_uart esp_driver_gpio esp_timer)
