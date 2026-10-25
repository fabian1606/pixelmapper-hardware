/*
 * pm_board.c - revision detection, early GPIO state (ESP-IDF part of pm_board).
 *
 * The tables themselves are in pm_board_tables.c (pure C, host-tested for double-booked GPIOs).
 */
#include "pm_board.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "pm_board";

/* Revision code pins (rev >= 1). Rev 0 has none; the own PCB must put its code on pins that float on rev 0
 * (see WIRING.md "Revision"). Until rev 1 exists the list is empty and auto-detection always yields rev 0. */
#define REV_CODE_N 0u
static const int8_t s_rev_code_pins[PM_BOARD_REV_CODE_MAX_PINS] = {
    PM_BOARD_PIN_NONE, PM_BOARD_PIN_NONE, PM_BOARD_PIN_NONE, PM_BOARD_PIN_NONE};

static const pm_board_t *s_board; /* NULL until pm_board_init: pm_board_get() falls back to rev 0 */

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
    const pm_board_t *b = pm_board_table(rev);
    if (b == NULL) {
        ESP_LOGE(TAG, "no pin table for hardware revision %u, using rev 0 pins", rev);
        b = pm_board_table(0);
        err = PM_ERR_UNSUPPORTED;
    }
    int8_t dup;
    if (!pm_board_pins_unique(b, &dup)) {
        ESP_LOGE(TAG, "pin table of rev %u uses GPIO%d twice", b->revision, dup); /* host test catches this */
        err = PM_ERR_UNSUPPORTED;
    }
    s_board = b;
    early_gpio(b);
    ESP_LOGI(TAG, "%s", b->name);
    return err;
}

const pm_board_t *pm_board_get(void) { return s_board ? s_board : pm_board_table(0); }

uint8_t pm_board_revision(void) { return pm_board_get()->revision; }

uint32_t pm_board_supported_revisions(void)
{
    uint32_t mask = 0;
    for (unsigned rev = 0; rev < 32; rev++) {
        if (pm_board_table((uint8_t)rev)) mask |= 1u << rev;
    }
    return mask;
}
