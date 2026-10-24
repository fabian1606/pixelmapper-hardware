/* DMX personalities (RDM DMX_PERSONALITY). */
#pragma once
#include <stdint.h>
#include "config.h"

#define PERS_COUNT 4
/* 1: N x RGB, 2: RGB whole tube, 3: RGB + master, 4: N x RGB + master */

uint8_t pers_footprint(uint8_t p);                 /* 0 = invalid */
const char *pers_description(uint8_t p);           /* PROGMEM string */
/* Slot layout of personality p (same order as pers_map): slot s = 0..footprint-1.
 * Label IDs per E1.37-1 (SD_*); 0 = invalid. */
#define SD_INTENSITY       0x0001
#define SD_COLOR_ADD_RED   0x0205
#define SD_COLOR_ADD_GREEN 0x0206
#define SD_COLOR_ADD_BLUE  0x0207
uint16_t pers_slot_label(uint8_t p, uint8_t s);
uint8_t pers_slot_default(uint8_t p, uint8_t s);   /* 255 dimmer, 0 colour */
/* writes ASCII description (no NUL, max 11 chars) into out, returns length */
uint8_t pers_slot_desc(uint8_t p, uint8_t s, char *out);

void pers_map(uint8_t p, const uint8_t *dmx, uint8_t lv[LED_CHANNELS]);
