#include "settings.h"
#include "personality.h"
#include <avr/eeprom.h>
#include <avr/io.h>
#include <avr/wdt.h>
#include <util/crc16.h>

settings_t g_set;
static uint8_t dirty;

/* uninitialised RAM content at power-up = entropy for the device ID */
static uint8_t noinit_ram[32] __attribute__((section(".noinit")));

static uint8_t crc8(const uint8_t *p, uint8_t n)
{
    uint8_t c = 0;
    while (n--) c = _crc8_ccitt_update(c, *p++);
    return c;
}

static void set_crc(void)
{
    g_set.crc = crc8((const uint8_t *)&g_set, sizeof(g_set) - 1);
}

void settings_load(void)
{
    eeprom_read_block(&g_set, (const void *)EE_ADDR_SETTINGS, sizeof(g_set));
    if (g_set.magic != SETTINGS_MAGIC || g_set.version != SETTINGS_VERSION ||
        g_set.crc != crc8((const uint8_t *)&g_set, sizeof(g_set) - 1) ||
        !pers_footprint(g_set.personality) || g_set.label_len > DEVICE_LABEL_MAX ||
        g_set.start_address < 1 ||
        g_set.start_address + pers_footprint(g_set.personality) - 1 > 512) {
        for (uint8_t i = 0; i < sizeof(g_set); i++) ((uint8_t *)&g_set)[i] = 0;
        g_set.magic = SETTINGS_MAGIC;
        g_set.version = SETTINGS_VERSION;
        g_set.start_address = DEFAULT_START_ADDRESS;
        g_set.personality = DEFAULT_PERSONALITY;
        settings_changed();
    }
}

void settings_changed(void)
{
    set_crc();
    dirty = 1;
}

void settings_poll(void)
{
    if (!dirty || !eeprom_is_ready()) return;
    const uint8_t *s = (const uint8_t *)&g_set;
    for (uint8_t i = 0; i < sizeof(g_set); i++) {
        uint8_t *a = (uint8_t *)(EE_ADDR_SETTINGS + i);
        if (eeprom_read_byte(a) != s[i]) {
            eeprom_write_byte(a, s[i]);   /* starts write, returns at once */
            return;
        }
    }
    dirty = 0;
}

void settings_flush(void)
{
    while (dirty) {                         /* up to ~40 bytes * 8.5 ms */
        wdt_reset();
        settings_poll();
    }
}

static uint32_t mix(uint32_t h, uint8_t b)
{
    h ^= b;
    h *= 16777619UL;                        /* FNV-1a */
    return h;
}

uint32_t devid_load(void)
{
    uint32_t id = 0;
    for (uint8_t i = 0; i < 4; i++)
        id = (id << 8) | eeprom_read_byte((const uint8_t *)(EE_ADDR_DEVICE_ID + i));
    if (id != 0xFFFFFFFFUL && id != 0) return id;

    /* blank EEPROM: derive a pseudo-random ID once and store it */
    uint32_t h = 2166136261UL;
    for (uint8_t i = 0; i < sizeof(noinit_ram); i++) h = mix(h, noinit_ram[i]);
    ADMUX = (1 << REFS0) | ADC_NOISE_CHANNEL;
    ADCSRA = (1 << ADEN) | (1 << ADPS2) | (1 << ADPS1);   /* /64 */
    for (uint8_t i = 0; i < 64; i++) {
        ADCSRA |= (1 << ADSC);
        while (ADCSRA & (1 << ADSC))
            ;
        h = mix(h, ADCL);
        h = mix(h, ADCH);
        h = mix(h, TCNT1L);                   /* PWM timer phase */
        h = mix(h, TCNT2);
        ADMUX ^= (1 << REFS1);                /* alternate AVCC / 2.56 V ref */
    }
    ADCSRA = 0;
    if (h == 0xFFFFFFFFUL || h == 0) h = 0x00000001UL;
    for (uint8_t i = 0; i < 4; i++) {
        eeprom_write_byte((uint8_t *)(EE_ADDR_DEVICE_ID + i), (uint8_t)(h >> (24 - 8 * i)));
        wdt_reset();
    }
    return h;
}
