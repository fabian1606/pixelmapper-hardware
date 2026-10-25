# Host tests for the pure-C components (pm_protocol, pm_artnet, pm_rdm) - no ESP-IDF needed.
#   cmake -S node/firmware/test_host -B node/firmware/test_host/build && cmake --build node/firmware/test_host/build
#   ctest --test-dir node/firmware/test_host/build --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(pm_test_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(FW ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(REPO ${FW}/../..)
set(VECTORS ${REPO}/protocol/vectors)
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

set(PM_WARN -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes)

add_library(pm_protocol STATIC ${FW}/components/pm_protocol/src/pm_types.c
                               ${FW}/components/pm_protocol/src/pm_settings.c
                               ${FW}/components/pm_protocol/src/pm_fw.c)
target_include_directories(pm_protocol PUBLIC ${FW}/components/pm_protocol/include)
target_compile_options(pm_protocol PRIVATE ${PM_WARN})
target_compile_definitions(pm_protocol PUBLIC _DEFAULT_SOURCE)

add_library(pm_artnet STATIC ${FW}/components/pm_artnet/src/pm_artnet_packets.c
                             ${FW}/components/pm_artnet/src/pm_artnet_node.c)
target_include_directories(pm_artnet PUBLIC ${FW}/components/pm_artnet/include)
target_link_libraries(pm_artnet PUBLIC pm_protocol)
target_compile_options(pm_artnet PRIVATE ${PM_WARN})

add_library(pm_rdm STATIC ${FW}/components/pm_rdm/src/pm_rdm.c
                          ${FW}/components/pm_rdm/src/pm_rdm_ctrl.c
                          ${FW}/components/pm_rdm/src/pm_rdm_responder.c)
target_include_directories(pm_rdm PUBLIC ${FW}/components/pm_rdm/include)
target_link_libraries(pm_rdm PUBLIC pm_protocol)
target_compile_options(pm_rdm PRIVATE ${PM_WARN})

add_library(pm_node STATIC ${FW}/components/pm_node/src/pm_node.c)
target_include_directories(pm_node PUBLIC ${FW}/components/pm_node/include)
target_link_libraries(pm_node PUBLIC pm_artnet pm_rdm pm_protocol)
target_compile_options(pm_node PRIVATE ${PM_WARN})

file(GLOB_RECURSE VECTOR_FILES CONFIGURE_DEPENDS ${VECTORS}/*.json)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/vectors_gen.c
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/vectors_to_c.py ${VECTORS}
            ${CMAKE_CURRENT_BINARY_DIR}/vectors_gen.c
    DEPENDS ${VECTOR_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/tools/vectors_to_c.py
    COMMENT "Converting protocol/vectors to C")

# Simulated RS-485 bus with virtual responders (pm_rdm_ctrl tests, virtual node).
add_library(pm_sim STATIC sim/sim_bus.c sim/sim_model.c)
target_include_directories(pm_sim PUBLIC sim)
target_link_libraries(pm_sim PUBLIC pm_rdm pm_protocol)
target_compile_options(pm_sim PRIVATE ${PM_WARN})

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS src/*.c)  # test_<component>*.c, see CLAUDE.md
add_executable(pm_tests ${TEST_SOURCES}
                        ${CMAKE_CURRENT_BINARY_DIR}/vectors_gen.c)
target_include_directories(pm_tests PRIVATE src)
target_link_libraries(pm_tests PRIVATE pm_node pm_sim)
target_compile_options(pm_tests PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME pm_tests COMMAND pm_tests)

# Virtual node: the real pm_node/pm_artnet/pm_rdm with fake port engine (simulated bus), RAM config and RAM
# firmware sink (pm_ota_session) on real UDP sockets - target of tools/e2e.sh (pixelmapper-net end-to-end tests).
add_executable(virtual_node virtual_node/vn_main.c virtual_node/vn_port.c virtual_node/vn_config.c
                            virtual_node/vn_fw.c
                            ${FW}/components/pm_config/src/pm_config_store.c
                            ${FW}/components/pm_ota/src/pm_ota_session.c)
target_include_directories(virtual_node PRIVATE virtual_node ${FW}/components/pm_config/src
                           ${FW}/components/pm_ota/src)
target_link_libraries(virtual_node PRIVATE pm_node pm_sim)
target_compile_options(virtual_node PRIVATE ${PM_WARN})
target_compile_definitions(virtual_node PRIVATE _DEFAULT_SOURCE _DARWIN_C_SOURCE)
add_test(NAME virtual_node_smoke COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/vn_smoke.py
         $<TARGET_FILE:virtual_node>)

# Component-owned host test projects, run in the same ctest pass (Phase 2 integration).
add_subdirectory(${FW}/components/pm_port/test_host ${CMAKE_CURRENT_BINARY_DIR}/pm_port)   # test_pm_port
add_subdirectory(${FW}/main/test_host ${CMAKE_CURRENT_BINARY_DIR}/pm_net)                  # pm_net_tests

# Front-panel host renderer (pm_ui): smoke test, plays all scenes and writes PNG/PPM into the build dir.
set(UI ${FW}/components/pm_ui)
add_executable(ui_host ${UI}/host/host_main.c ${UI}/host/scenes.c
                       ${UI}/src/ui_core.c ${UI}/src/ui_gfx.c ${UI}/src/ui_input.c ${UI}/src/ui_rdm.c
                       ${UI}/src/ui_font_data.c)
target_include_directories(ui_host PRIVATE ${UI}/host/stubs ${UI}/include ${UI}/src
                           ${FW}/components/pm_board/include ${FW}/components/pm_port/include
                           ${FW}/components/pm_rdm/include)
target_link_libraries(ui_host PRIVATE pm_protocol)
target_compile_options(ui_host PRIVATE -Wall -Wextra -Werror -Wshadow)
add_test(NAME pm_ui_render COMMAND ui_host ${CMAKE_CURRENT_BINARY_DIR}/ui_out
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/ui_out)

# Random/mutation test of the network-facing parsers (fuzz/fuzz_main.c): own copies of the component sources, built
# with AddressSanitizer + UndefinedBehaviorSanitizer where the compiler supports them (AppleClang, Clang, GCC).
option(PM_FUZZ_SANITIZE "build pm_fuzz with -fsanitize=address,undefined" ON)
set(PM_FUZZ_SOURCES fuzz/fuzz_main.c virtual_node/vn_fw.c
    ${FW}/components/pm_protocol/src/pm_types.c ${FW}/components/pm_protocol/src/pm_settings.c
    ${FW}/components/pm_protocol/src/pm_fw.c
    ${FW}/components/pm_artnet/src/pm_artnet_packets.c ${FW}/components/pm_artnet/src/pm_artnet_node.c
    ${FW}/components/pm_rdm/src/pm_rdm.c ${FW}/components/pm_rdm/src/pm_rdm_ctrl.c
    ${FW}/components/pm_rdm/src/pm_rdm_responder.c ${FW}/components/pm_node/src/pm_node.c
    ${FW}/components/pm_ota/src/pm_ota_session.c)
add_executable(pm_fuzz ${PM_FUZZ_SOURCES})
target_include_directories(pm_fuzz PRIVATE virtual_node sim ${FW}/components/pm_protocol/include
                           ${FW}/components/pm_artnet/include ${FW}/components/pm_rdm/include
                           ${FW}/components/pm_node/include ${FW}/components/pm_ota/src)
target_compile_definitions(pm_fuzz PRIVATE _DEFAULT_SOURCE)
target_compile_options(pm_fuzz PRIVATE ${PM_WARN} -g -O1)
if(PM_FUZZ_SANITIZE AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pm_fuzz PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all
                           -fno-omit-frame-pointer)
    target_link_options(pm_fuzz PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME pm_fuzz COMMAND pm_fuzz 100000 1)
