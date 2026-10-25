# Host tests for the pure-C parts of pm_net, pm_config and pm_ota (no ESP-IDF needed).
#   cmake -S node/firmware/main/test_host -B node/firmware/main/test_host/build -G Ninja
#   cmake --build node/firmware/main/test_host/build && ctest --test-dir node/firmware/main/test_host/build --output-on-failure
# Separate from node/firmware/test_host (architect-owned); reuses its test.h.
cmake_minimum_required(VERSION 3.16)
project(pm_net_test_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(FW ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(PM_WARN -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes)

add_library(pm_protocol_h STATIC ${FW}/components/pm_protocol/src/pm_types.c
                                 ${FW}/components/pm_protocol/src/pm_settings.c
                                 ${FW}/components/pm_protocol/src/pm_fw.c)
target_include_directories(pm_protocol_h PUBLIC ${FW}/components/pm_protocol/include)
target_compile_definitions(pm_protocol_h PUBLIC _DEFAULT_SOURCE)

add_executable(pm_net_tests
    test_main.c
    ${FW}/components/pm_net/src/pm_net_ip.c
    ${FW}/components/pm_net/test/test_net_ip.c
    ${FW}/components/pm_config/src/pm_config_store.c
    ${FW}/components/pm_config/test/test_config_store.c
    ${FW}/components/pm_ota/src/pm_ota_session.c
    ${FW}/components/pm_ota/test/test_ota_session.c)
target_include_directories(pm_net_tests PRIVATE
    ${FW}/test_host/src
    ${FW}/components/pm_net/src
    ${FW}/components/pm_net/include
    ${FW}/components/pm_config/src
    ${FW}/components/pm_ota/src)
target_link_libraries(pm_net_tests PRIVATE pm_protocol_h)
target_compile_options(pm_net_tests PRIVATE ${PM_WARN})

enable_testing()
add_test(NAME pm_net_tests COMMAND pm_net_tests)
