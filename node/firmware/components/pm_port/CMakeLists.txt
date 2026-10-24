idf_component_register(SRCS "src/pm_port.c" "core/pm_port_line.c" "core/pm_port_tbuf.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "core"
                       REQUIRES pm_board pm_rdm pm_protocol esp_common
                       PRIV_REQUIRES pm_artnet esp_driver_uart esp_driver_gpio esp_driver_gptimer esp_timer
                                     hal soc log freertos)
