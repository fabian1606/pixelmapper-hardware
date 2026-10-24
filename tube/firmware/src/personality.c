#include "personality.h"
#include <avr/pgmspace.h>
#include <string.h>

#define STR2(x) #x
#define STR(x) STR2(x)

static const char d1[] PROGMEM = STR(LED_SEGMENTS) "x RGB";
static const char d2[] PROGMEM = "RGB";
static const char d3[] PROGMEM = "RGB + Dimmer";
static const char d4[] PROGMEM = STR(LED_SEGMENTS) "x RGB + Dimmer";
static PGM_P const desc[PERS_COUNT] PROGMEM = { d1, d2, d3, d4 };

uint8_t pers_footprint(uint8_t p)
{
    switch (p) {
    case 1: return 3 * LED_SEGMENTS;
    case 2: return 3;
    case 3: return 4;
    case 4: return 3 * LED_SEGMENTS + 1;
    default: return 0;
    }
}

const char *pers_description(uint8_t p)
{
    return (const char *)pgm_read_word(&desc[p - 1]);
}

/* Slot layout - mirrors pers_map(): personalities 3/4 carry the dimmer as the
 * LAST slot; colours cycle R,G,B (p1/p4 per segment, p2/p3 whole tube). */
static const uint16_t slot_rgb[3] PROGMEM = {
    SD_COLOR_ADD_RED, SD_COLOR_ADD_GREEN, SD_COLOR_ADD_BLUE
};
static const char n_red[]   PROGMEM = "Red";
static const char n_green[] PROGMEM = "Green";
static const char n_blue[]  PROGMEM = "Blue";
static const char n_dim[]   PROGMEM = "Dimmer";
static PGM_P const slot_name[3] PROGMEM = { n_red, n_green, n_blue };

static uint8_t slot_is_dimmer(uint8_t p, uint8_t s)
{
    return (p == 3 || p == 4) && s == pers_footprint(p) - 1;
}

uint16_t pers_slot_label(uint8_t p, uint8_t s)
{
    if (s >= pers_footprint(p)) return 0;
    if (slot_is_dimmer(p, s)) return SD_INTENSITY;
    return pgm_read_word(&slot_rgb[s % 3]);
}

uint8_t pers_slot_default(uint8_t p, uint8_t s)
{
    return slot_is_dimmer(p, s) ? 255 : 0;
}

uint8_t pers_slot_desc(uint8_t p, uint8_t s, char *out)
{
    uint8_t n = 0;
    PGM_P name;
    if (slot_is_dimmer(p, s)) {
        name = n_dim;
    } else {
        if (p == 1 || p == 4) {
            memcpy_P(out, PSTR("Seg "), 4);
            out[4] = '1' + s / 3;
            out[5] = ' ';
            n = 6;
        }
        name = (PGM_P)pgm_read_word(&slot_name[s % 3]);
    }
    uint8_t l = strlen_P(name);
    memcpy_P(out + n, name, l);
    return n + l;
}

static uint8_t scale(uint8_t v, uint8_t m)
{
    uint16_t x = (uint16_t)v * m + 128;
    return (x + (x >> 8)) >> 8;           /* v * m / 255, rounded */
}

void pers_map(uint8_t p, const uint8_t *dmx, uint8_t lv[LED_CHANNELS])
{
    uint8_t master = 255;
    if (p == 3) master = dmx[3];
    if (p == 4) master = dmx[3 * LED_SEGMENTS];
    for (uint8_t c = 0; c < LED_CHANNELS; c++) {
        uint8_t v = (p == 2 || p == 3) ? dmx[c % 3] : dmx[c];
        lv[c] = scale(v, master);
    }
}
