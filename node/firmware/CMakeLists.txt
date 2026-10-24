# Pixelmapper Node firmware (ESP-IDF, target esp32p4)
#   . ~/esp/esp-idf/export.sh && idf.py set-target esp32p4 && idf.py build
cmake_minimum_required(VERSION 3.16)

set(PM_FW_VERSION_MAJOR 0)
set(PM_FW_VERSION_MINOR 1)
set(PM_FW_VERSION_PATCH 0)
set(PROJECT_VER "${PM_FW_VERSION_MAJOR}.${PM_FW_VERSION_MINOR}.${PM_FW_VERSION_PATCH}")
add_compile_definitions(
    PM_FW_VERSION_MAJOR=${PM_FW_VERSION_MAJOR}
    PM_FW_VERSION_MINOR=${PM_FW_VERSION_MINOR}
    PM_FW_VERSION_PATCH=${PM_FW_VERSION_PATCH})

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pixelmapper_node)
