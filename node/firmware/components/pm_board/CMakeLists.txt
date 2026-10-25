idf_component_register(SRCS "src/pm_board.c" "src/pm_board_rev.c"
                       INCLUDE_DIRS "include"
                       REQUIRES pm_protocol
                       PRIV_REQUIRES esp_driver_gpio esp_rom log)
