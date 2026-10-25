# Host tests of the hardware-independent part of pm_port (line FSM, frame scheduler, triple buffer)
# with the real pm_rdm_ctrl (RDM gate), the pm_board revision-code decoder and the pin-table self-check.
#
#   cmake -S node/firmware/components/pm_port/test_host -B node/firmware/components/pm_port/test_host/build -G Ninja
#   cmake --build node/firmware/components/pm_port/test_host/build
#   ctest --test-dir node/firmware/components/pm_port/test_host/build --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(pm_port_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(COMP ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(COMPONENTS ${COMP}/..)

add_executable(test_pm_port
    test_pm_port.c
    ${COMP}/core/pm_port_line.c
    ${COMP}/core/pm_port_tbuf.c
    ${COMPONENTS}/pm_board/src/pm_board_rev.c
    ${COMPONENTS}/pm_board/src/pm_board_tables.c
    ${COMPONENTS}/pm_rdm/src/pm_rdm.c
    ${COMPONENTS}/pm_rdm/src/pm_rdm_ctrl.c
    ${COMPONENTS}/pm_protocol/src/pm_types.c)
target_include_directories(test_pm_port PRIVATE
    ${COMP}/core
    ${COMPONENTS}/pm_board/include
    ${COMPONENTS}/pm_rdm/include
    ${COMPONENTS}/pm_protocol/include)
target_compile_options(test_pm_port PRIVATE -Wall -Wextra -Werror -Wshadow)

enable_testing()
add_test(NAME pm_port COMMAND test_pm_port)
