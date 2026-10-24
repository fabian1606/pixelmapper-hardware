/*
 * pm_board.h - board abstraction: one pin table per hardware revision, revision detection at boot.
 *
 * One firmware for all revisions (docs/plan.md "Board-Abstraktion"). Rev 0 = prototype: Waveshare
 * ESP32-P4-WIFI6-POE-ETH + hand-wired MAX3485/SP3485 (3.3 V), ST7735S display, encoder, BACK button.
 * Later revisions (own PCB) encode their number with resistors on GPIOs; the prototype has no code and is
 * identified as rev 0 unless pm_settings_t.hw_revision_override says otherwise.
 *
 * All other components take their pins from pm_board_get() - never hard-code GPIO numbers elsewhere.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PM_BOARD_PIN_NONE (-1)

/** RS-485 transceiver of one DMX port. */
typedef struct {
    int8_t uart_num;   /**< UART peripheral (1..4 on the P4; UART0 is the console) */
    int8_t tx;         /**< DI */
    int8_t rx;         /**< RO */
    int8_t de;         /**< DE (+ /RE tied to DE, or separate re pin) - driven by the UART RS-485 mode (RTS) */
    int8_t re;         /**< separate /RE or PM_BOARD_PIN_NONE */
} pm_board_port_pins_t;

typedef struct {
    int8_t spi_host;   /**< SPI2_HOST/SPI3_HOST as int */
    int8_t sclk, mosi, cs, dc, rst, backlight; /**< backlight: PWM (LEDC) */
    uint8_t rotation;  /**< mounting orientation 0..3 (landscape variants) */
    bool backlight_active_high;
} pm_board_display_pins_t;

typedef struct {
    int8_t enc_a, enc_b, enc_push, back; /**< active low, internal pull-ups */
} pm_board_input_pins_t;

typedef struct {
    int8_t mdc, mdio, phy_reset;
    int8_t phy_addr;
    int8_t rmii_clk_in;    /**< 50 MHz REF_CLK from the PHY/oscillator, PM_BOARD_PIN_NONE = P4 outputs it */
} pm_board_eth_pins_t;

typedef struct {
    uint8_t revision;
    const char *name;                       /**< "Rev 0 (Waveshare P4-ETH prototype)" */
    uint8_t num_ports;                      /**< <= PM_NUM_PORTS */
    pm_board_port_pins_t port[PM_NUM_PORTS];
    pm_board_display_pins_t display;
    pm_board_input_pins_t input;
    pm_board_eth_pins_t eth;
    bool has_c6;                            /**< ESP32-C6 over SDIO present (kept off) */
    int8_t c6_enable;                       /**< pin that holds the C6 in reset, PM_BOARD_PIN_NONE if none */
} pm_board_t;

/** Detect the revision (resistor code or override) and select the pin table. Call once, first thing in main.
 *  hw_revision_override = 0xFF -> auto-detect. Returns PM_ERR_UNSUPPORTED for an unknown revision code (the
 *  firmware then runs with rev 0 pins and shows an error). */
pm_err_t pm_board_init(uint8_t hw_revision_override);

/** The active board description (valid after pm_board_init; rev 0 before). Never NULL. */
const pm_board_t *pm_board_get(void);

/** Detected hardware revision (NODE_HW_REVISION, image hw_rev_mask bit). */
uint8_t pm_board_revision(void);

/** Bitmask of all revisions this firmware has pin tables for (-> image description hw_rev_mask). */
uint32_t pm_board_supported_revisions(void);

#ifdef __cplusplus
}
#endif
