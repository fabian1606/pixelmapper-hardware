/*
 * tube_fw - DMX512 receiver + RDM responder for the ATmega8 in the LED tube.
 * See include/config.h for all hardware settings.
 */
#include "config.h"
#include "dmx.h"
#include "output.h"
#include "personality.h"
#include "rdm.h"
#include "settings.h"
#include "timebase.h"
#if GAMMA_ENABLE
#include "gamma.h"
#endif
#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <string.h>

#ifdef BOOT_LAYOUT_2K
#include "boot_shared.h"
/* Trailer metadata at 0x17F0 (boot_shared.h): own section, placed and kept
 * by extra_script.py (--section-start, -u). Length + CRC behind it are added
 * by the post-build step -> tube_fw-<version>.hex. */
const app_trailer_meta_t app_trailer_meta __attribute__((section(".trailer"), used)) = {
    .magic = TRAILER_MAGIC,
    .format = TRAILER_FORMAT,
    .reserved = 0xFF,
    .manufacturer = RDM_MANUFACTURER_ID,
    .model = RDM_DEVICE_MODEL_ID,
    .fw_version = FW_VERSION_ID,
};
#endif

static uint8_t lv[LED_CHANNELS];

static void fill(uint8_t v)
{
    memset(lv, v, sizeof(lv));
}

static void show(void)
{
    uint8_t o[LED_CHANNELS];
    for (uint8_t i = 0; i < LED_CHANNELS; i++)
#if GAMMA_ENABLE
        o[i] = pgm_read_byte(&gamma_table[lv[i]]);
#else
        o[i] = lv[i];
#endif
    out_write(o);
}

static void selftest(void)
{
#if SELFTEST_ENABLE
    /* segment 1 R, G, B, segment 2 R, G, B ... -> verify pinout visually */
    for (uint8_t c = 0; c < LED_CHANNELS; c++) {
        fill(0);
        lv[c] = SELFTEST_LEVEL;
        show();
        tb_wait_ms(SELFTEST_STEP_MS);
    }
    fill(0);
    show();
#endif
}

#ifdef SELFTEST_LOOP
/* Bench test build (env:selftest): endless pattern through the normal BCM
 * driver instead of DMX operation. tb_wait_ms() kicks the watchdog. */
static void fill_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    for (uint8_t s = 0; s < LED_SEGMENTS; s++) {
        lv[3 * s] = r;
        lv[3 * s + 1] = g;
        lv[3 * s + 2] = b;
    }
}

static void hold(uint16_t ms)
{
    show();
    tb_wait_ms(ms);
}

static void __attribute__((noreturn)) selftest_loop(void)
{
    for (;;) {
        /* 1. each channel alone: R1 G1 B1 R2 ... B4, 1 s on / 0.3 s off */
        for (uint8_t c = 0; c < LED_CHANNELS; c++) {
            fill(0);
            lv[c] = 255;
            hold(1000);
            fill(0);
            hold(300);
        }
        /* 2. all segments red, green, blue, white, 1 s each */
        fill_rgb(255, 0, 0);     hold(1000);
        fill_rgb(0, 255, 0);     hold(1000);
        fill_rgb(0, 0, 255);     hold(1000);
        fill(255);               hold(1000);
        /* 3. chaser: white on segment 1..4, 0.5 s each */
        for (uint8_t s = 0; s < LED_SEGMENTS; s++) {
            fill(0);
            memset(&lv[3 * s], 255, 3);
            hold(500);
        }
        /* 4. white fade up + down over all segments, ~2 s (PWM check) */
        for (uint16_t i = 0; i < 512; i++) {
            fill(i < 256 ? (uint8_t)i : (uint8_t)(511 - i));
            hold(4);
        }
        fill(0);
        hold(300);
    }
}
#endif

int main(void)
{
    wdt_reset();
    wdt_enable(WDT_TIMEOUT);
    MCUCSR = 0;
    rs485_init();                       /* transceiver to receive before the self-test */

    tb_init();
    out_init();
    sei();

#ifdef SELFTEST_LOOP
    selftest_loop();
#endif

    settings_load();
    uint32_t id = devid_load();
    rdm_init(id);
    selftest();

    dmx_init();
    dmx_configure(g_set.start_address, pers_footprint(g_set.personality));

    uint8_t dmx[DMX_MAX_FOOTPRINT];
    uint8_t dmx_lv[LED_CHANNELS];
    uint8_t have_dmx = 0;
    uint16_t last_frame = 0, last_out = 0;
    memset(dmx_lv, 0, sizeof(dmx_lv));

    for (;;) {
        wdt_reset();
        rdm_poll();

        uint16_t now = tb_now();
        uint8_t upd = 0;
        if (dmx_take_frame(dmx)) {
            pers_map(g_set.personality, dmx, dmx_lv);
            have_dmx = 1;
            last_frame = now;
            upd = 1;
        }
        if (have_dmx && (uint16_t)(now - last_frame) > DMX_LOSS_TIMEOUT_MS)
            have_dmx = 0;
        if ((uint16_t)(now - last_out) >= 20)
            upd = 1;

        if (upd) {
            last_out = now;
            if (rdm_identify)
                fill((now & 0x100) ? IDENTIFY_LEVEL : 0);    /* ~2 Hz blink */
            else if (have_dmx || DMX_LOSS_MODE == DMX_LOSS_HOLD)
                memcpy(lv, dmx_lv, sizeof(lv));
            else
                fill(DMX_LOSS_MODE == DMX_LOSS_WHITE ? DMX_LOSS_WHITE_LEVEL : 0);
            show();
        }

        settings_poll();
    }
}
