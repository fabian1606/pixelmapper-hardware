/*
 * pm_board.c - pin tables. Rev 0 values are PLACEHOLDERS marked TODO(pinout) until the Waveshare schematic is
 * checked and the prototype is wired (docs/plan.md "Offene Punkte"). Ethernet pins follow the ESP-IDF ethernet
 * example defaults for ESP32-P4 (IP101, MDC 31, MDIO 52, reset 51, addr 1) - verify against the Waveshare schematic.
 */
#include "pm_board.h"

static const pm_board_t s_rev0 = {
    .revision = 0,
    .name = "Rev 0 (Waveshare P4-ETH prototype)",
    .num_ports = 4,
    .port = {
        /* TODO(pinout): free GPIOs from the Waveshare pin header */
        {.uart_num = 1, .tx = 2, .rx = 3, .de = 4, .re = PM_BOARD_PIN_NONE},
        {.uart_num = 2, .tx = 5, .rx = 6, .de = 7, .re = PM_BOARD_PIN_NONE},
        {.uart_num = 3, .tx = 20, .rx = 21, .de = 22, .re = PM_BOARD_PIN_NONE},
        {.uart_num = 4, .tx = 23, .rx = 24, .de = 25, .re = PM_BOARD_PIN_NONE},
    },
    /* TODO(pinout) */
    .display = {.spi_host = 1, .sclk = 26, .mosi = 27, .cs = 28, .dc = 29, .rst = 30, .backlight = 32,
                .rotation = 1, .backlight_active_high = true},
    .input = {.enc_a = 33, .enc_b = 36, .enc_push = 46, .back = 47}, /* TODO(pinout) */
    .eth = {.mdc = 31, .mdio = 52, .phy_reset = 51, .phy_addr = 1, .rmii_clk_in = 50},
    .has_c6 = true,
    .c6_enable = PM_BOARD_PIN_NONE, /* TODO(pinout): C6 reset/enable pin */
};

static const pm_board_t *s_board = &s_rev0;

pm_err_t pm_board_init(uint8_t hw_revision_override)
{
    /* Phase 1: read the resistor code (own PCB) when no override is set. The prototype has none -> rev 0. */
    if (hw_revision_override != 0xFF && hw_revision_override != 0) {
        s_board = &s_rev0;
        return PM_ERR_UNSUPPORTED;
    }
    s_board = &s_rev0;
    return PM_OK;
}

const pm_board_t *pm_board_get(void) { return s_board; }

uint8_t pm_board_revision(void) { return s_board->revision; }

uint32_t pm_board_supported_revisions(void) { return 1u << 0; }
