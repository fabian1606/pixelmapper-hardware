# Front panel. ui_core/ui_gfx/ui_input/ui_rdm/ui_font_data are pure C (also built by host/, the PNG renderer);
# pm_ui.c and lcd_st7735.c are the ESP-IDF glue.
idf_component_register(SRCS "src/pm_ui.c" "src/lcd_st7735.c" "src/ui_core.c" "src/ui_gfx.c" "src/ui_input.c"
                            "src/ui_rdm.c" "src/ui_font_data.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       REQUIRES pm_board pm_port pm_protocol esp_common
                       PRIV_REQUIRES pm_config pm_rdm esp_driver_spi esp_driver_gpio esp_driver_ledc
                                     esp_driver_pcnt esp_timer)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
