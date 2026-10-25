/*
 * pm_board_tables.c - pin tables of all hardware revisions + the double-booking check (pure C11, no ESP-IDF;
 * host-tested in pm_port/test_host).
 *
 * Rev 0 = Waveshare ESP32-P4-WIFI6-POE-ETH + hand-wired transceivers/display/encoder, see ../WIRING.md.
 * Pin sources (2026-10-24): Waveshare schematic ESP32-P4-WIFI6-POE-ETH-Schematic.pdf (header P6, IP101GRI U11,
 * C6/SD/codec nets) and the Waveshare ethernet demo Kconfig (waveshareteam/ESP32-P4-Platform, 11_ethernetbasic).
 * None of this is verified on hardware yet.
 */
#include <stddef.h>

#include "pm_board.h"

/*
 * Rev 0 pin plan. Header P6 pin numbers in brackets (Waveshare numbering: pin 1 = 5V, pin 2 = 3V3).
 * The header has only 13 GPIOs without any other use (2,3,4,5,20,21,22,23,32,33,46,47,48); the plan needs 21:
 *  - GPIO24/25 and 26/27 are the two internal USB-FS PHYs (USB-Serial-JTAG, OTG1.1). The board does not use them
 *    (console/flash = CH343 on UART0, OTG port = HS PHY). D+ (25, 27) carries the USB pull-up after reset, so
 *    only TX lines (idle high anyway) go there; DE never does.
 *  - GPIO0/1 = 32 kHz crystal pads (10 M + 22 pF). Fine as button inputs as long as the RTC uses the internal
 *    RC (CONFIG_RTC_CLK_SRC_INT_RC, IDF default).
 *  - GPIO6 = C6_IO2 via R52 (0 R). Free while the C6 is held in reset (c6_enable low, done in pm_board_init).
 *  - GPIO36 = strapping pin (download mode needs it high): display CS with an external 10 k pull-up is the
 *    right default for the strap and keeps the display deselected during boot.
 *  - The display RST is not wired to a GPIO (RC reset + SWRESET), there is no pin left for it.
 * No GPIO is used twice, also not against the on-board RMII/PHY, C6 and USB nets (pm_board_pins_unique(),
 * host test test_board_pins in pm_port/test_host). The display sits on header-only pins (32, 36, 46, 48, 2);
 * none of them is an RMII pin of this board (RMII: 28-31, 34, 35, 49-52), see WIRING.md "Nicht anfassen".
 */
static const pm_board_t s_rev0 = {
    .revision = 0,
    .name = "Rev 0 (Waveshare P4-ETH prototype)",
    .num_ports = 4,
    .port = {
        /* UART1: TX GPIO25 [27], RX GPIO24 [28], DE GPIO33 [30] */
        {.uart_num = 1, .tx = 25, .rx = 24, .de = 33, .re = PM_BOARD_PIN_NONE},
        /* UART2: TX GPIO27 [37], RX GPIO47 [38], DE GPIO26 [32] */
        {.uart_num = 2, .tx = 27, .rx = 47, .de = 26, .re = PM_BOARD_PIN_NONE},
        /* UART3: TX GPIO20 [14], RX GPIO21 [12], DE GPIO22 [11] */
        {.uart_num = 3, .tx = 20, .rx = 21, .de = 22, .re = PM_BOARD_PIN_NONE},
        /* UART4: TX GPIO5 [15], RX GPIO4 [17], DE GPIO3 [20] */
        {.uart_num = 4, .tx = 5, .rx = 4, .de = 3, .re = PM_BOARD_PIN_NONE},
    },
    /* SPI2: SCLK GPIO48 [34], MOSI GPIO46 [35], CS GPIO36 [23], DC GPIO32 [25], BL GPIO2 [22] (LEDC PWM) */
    .display = {.spi_host = 1 /* SPI2_HOST */, .sclk = 48, .mosi = 46, .cs = 36, .dc = 32,
                .rst = PM_BOARD_PIN_NONE, .backlight = 2, .rotation = 1, .backlight_active_high = true},
    /* ENC A GPIO23 [8], ENC B GPIO6 [16], ENC push GPIO0 [24], BACK GPIO1 [21] - all switch to GND, pull-ups */
    .input = {.enc_a = 23, .enc_b = 6, .enc_push = 0, .back = 1},
    /* IP101GRI, checked against the Waveshare schematic + demo: identical to the ESP-IDF P4 defaults */
    .eth = {.mdc = 31, .mdio = 52, .phy_reset = 51, .phy_addr = 1, .rmii_clk_in = 50,
            .rmii_txd0 = 34, .rmii_txd1 = 35, .rmii_tx_en = 49,
            .rmii_rxd0 = 29, .rmii_rxd1 = 30, .rmii_crs_dv = 28},
    .has_c6 = true,
    .c6_enable = 54, /* C6_CHIP_PU via R54; low = C6 held in reset */
};


static const pm_board_t *const s_tables[] = {&s_rev0};

const pm_board_t *pm_board_table(uint8_t revision)
{
    for (size_t i = 0; i < sizeof s_tables / sizeof s_tables[0]; i++) {
        if (s_tables[i]->revision == revision) return s_tables[i];
    }
    return NULL;
}

/* Mark one pin; false if it was marked already. */
static bool take(uint64_t *used, int8_t pin, int8_t *dup)
{
    if (pin < 0) return true;
    if (pin >= PM_BOARD_GPIO_MAX) {
        *dup = pin; /* not a GPIO of the P4: reported like a collision */
        return false;
    }
    uint64_t bit = 1ULL << (unsigned)pin;
    if (*used & bit) {
        *dup = pin;
        return false;
    }
    *used |= bit;
    return true;
}

bool pm_board_pins_unique(const pm_board_t *b, int8_t *dup_gpio)
{
    int8_t dummy;
    int8_t *dup = dup_gpio ? dup_gpio : &dummy;
    *dup = PM_BOARD_PIN_NONE;
    uint64_t used = 0;
    bool ok = true;
    for (unsigned i = 0; i < b->num_ports && i < PM_NUM_PORTS && ok; i++) {
        const pm_board_port_pins_t *p = &b->port[i];
        ok = take(&used, p->tx, dup) && take(&used, p->rx, dup) && take(&used, p->de, dup) &&
             take(&used, p->re, dup);
    }
    const pm_board_display_pins_t *d = &b->display;
    ok = ok && take(&used, d->sclk, dup) && take(&used, d->mosi, dup) && take(&used, d->cs, dup) &&
         take(&used, d->dc, dup) && take(&used, d->rst, dup) && take(&used, d->backlight, dup);
    const pm_board_input_pins_t *in = &b->input;
    ok = ok && take(&used, in->enc_a, dup) && take(&used, in->enc_b, dup) && take(&used, in->enc_push, dup) &&
         take(&used, in->back, dup);
    const pm_board_eth_pins_t *e = &b->eth;
    ok = ok && take(&used, e->mdc, dup) && take(&used, e->mdio, dup) && take(&used, e->phy_reset, dup) &&
         take(&used, e->rmii_clk_in, dup) && take(&used, e->rmii_txd0, dup) && take(&used, e->rmii_txd1, dup) &&
         take(&used, e->rmii_tx_en, dup) && take(&used, e->rmii_rxd0, dup) && take(&used, e->rmii_rxd1, dup) &&
         take(&used, e->rmii_crs_dv, dup);
    ok = ok && take(&used, b->c6_enable, dup);
    return ok;
}
