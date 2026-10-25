idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES pm_protocol pm_artnet pm_rdm pm_node pm_board pm_port pm_net pm_config pm_ota pm_ui
                                nvs_flash esp_timer esp_system heap freertos log)

# Post-build: the PMIM block must sit at image offset 288 and the app must fit into one OTA slot
# (image-format.md §2.1). OTA slot size = ota_0/ota_1 in partitions.csv (0x400000).
set(PM_OTA_SLOT_SIZE 0x400000)
idf_build_get_property(build_dir BUILD_DIR)
idf_build_get_property(python PYTHON)
idf_build_get_property(project_name PROJECT_NAME)
add_custom_target(pm_image_check ALL
    COMMAND ${python} ${CMAKE_CURRENT_LIST_DIR}/../components/pm_ota/tools/check_image.py
            ${build_dir}/${project_name}.bin --max-size ${PM_OTA_SLOT_SIZE}
    COMMENT "Checking pm_image_desc offset and OTA slot size"
    VERBATIM)
add_dependencies(pm_image_check gen_project_binary)
