idf_component_register(SRCS "src/pm_ui.c"
                       INCLUDE_DIRS "include"
                       REQUIRES pm_board pm_port pm_protocol esp_common
                       PRIV_REQUIRES esp_driver_spi esp_driver_gpio esp_driver_ledc esp_lcd)
