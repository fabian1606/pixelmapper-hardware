/*
 * pm_board.c - pin tables + revision detection.
 *
 * Rev 0 = Waveshare ESP32-P4-WIFI6-POE-ETH + hand-wired transceivers/display/encoder, see ../WIRING.md.
 * Pin sources (2026-10-24): Waveshare schematic ESP32-P4-WIFI6-POE-ETH-Schematic.pdf (header P6, IP101GRI U11,
 * C6/SD/codec nets) and the Waveshare ethernet demo Kconfig (waveshareteam/ESP32-P4-Platform, 11_ethernetbasic).
 * None of this is verified on hardware yet.
 */
#include "pm_board.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "pm_board";

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

/* Revision code pins (rev >= 1). Rev 0 has none; the own PCB must put its code on pins that float on rev 0
 * (see WIRING.md "Revision"). Until rev 1 exists the list is empty and auto-detection always yields rev 0. */
#define REV_CODE_N 0u
static const int8_t s_rev_code_pins[PM_BOARD_REV_CODE_MAX_PINS] = {
    PM_BOARD_PIN_NONE, PM_BOARD_PIN_NONE, PM_BOARD_PIN_NONE, PM_BOARD_PIN_NONE};

static const pm_board_t *const s_tables[] = {&s_rev0};

static const pm_board_t *s_board = &s_rev0;

static const pm_board_t *table_for(uint8_t rev)
{
    for (unsigned i = 0; i < sizeof s_tables / sizeof s_tables[0]; i++) {
        if (s_tables[i]->revision == rev) return s_tables[i];
    }
    return NULL;
}

static bool read_rev_code(const int8_t *pins, uint8_t n, uint8_t *rev)
{
    uint8_t up[PM_BOARD_REV_CODE_MAX_PINS] = {0}, down[PM_BOARD_REV_CODE_MAX_PINS] = {0};
    for (uint8_t i = 0; i < n && i < PM_BOARD_REV_CODE_MAX_PINS; i++) {
        gpio_num_t pin = (gpio_num_t)pins[i];
        gpio_reset_pin(pin);
        gpio_set_direction(pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
        esp_rom_delay_us(50);
        up[i] = (uint8_t)gpio_get_level(pin);
        gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);
        esp_rom_delay_us(50);
        down[i] = (uint8_t)gpio_get_level(pin);
        gpio_set_pull_mode(pin, GPIO_FLOATING);
    }
    return pm_board_decode_rev_code(up, down, n, rev);
}

/* Safe state as early as possible: transceivers receiving (DE low), C6 in reset. */
static void early_gpio(const pm_board_t *b)
{
    uint64_t out_mask = 0;
    for (unsigned i = 0; i < b->num_ports; i++) {
        if (b->port[i].de >= 0) out_mask |= 1ULL << b->port[i].de;
        if (b->port[i].re >= 0) out_mask |= 1ULL << b->port[i].re;
    }
    if (b->c6_enable >= 0) out_mask |= 1ULL << b->c6_enable;
    if (out_mask == 0) return;
    /* level first, then output enable: no glitch */
    for (unsigned i = 0; i < b->num_ports; i++) {
        if (b->port[i].de >= 0) gpio_set_level((gpio_num_t)b->port[i].de, 0);
        if (b->port[i].re >= 0) gpio_set_level((gpio_num_t)b->port[i].re, 0); /* /RE low = receive */
    }
    if (b->c6_enable >= 0) gpio_set_level((gpio_num_t)b->c6_enable, 0);
    gpio_config_t io = {
        .pin_bit_mask = out_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

pm_err_t pm_board_init(uint8_t hw_revision_override)
{
    pm_err_t err = PM_OK;
    uint8_t rev = 0;
    if (hw_revision_override != 0xFF) {
        rev = hw_revision_override;
    } else if (!read_rev_code(s_rev_code_pins, REV_CODE_N, &rev)) {
        ESP_LOGE(TAG, "invalid revision code, using rev 0 pins");
        rev = 0;
        err = PM_ERR_UNSUPPORTED;
    }
    const pm_board_t *b = table_for(rev);
    if (b == NULL) {
        ESP_LOGE(TAG, "no pin table for hardware revision %u, using rev 0 pins", rev);
        b = &s_rev0;
        err = PM_ERR_UNSUPPORTED;
    }
    s_board = b;
    early_gpio(b);
    ESP_LOGI(TAG, "%s", b->name);
    return err;
}

const pm_board_t *pm_board_get(void) { return s_board; }

uint8_t pm_board_revision(void) { return s_board->revision; }

uint32_t pm_board_supported_revisions(void)
{
    uint32_t mask = 0;
    for (unsigned i = 0; i < sizeof s_tables / sizeof s_tables[0]; i++) mask |= 1u << s_tables[i]->revision;
    return mask;
}
