/* Persistent settings (EEPROM, magic + version + CRC8) and device ID. */
#pragma once
#include <stdint.h>
#include "config.h"

typedef struct {
    uint8_t magic;
    uint8_t version;
    uint16_t start_address;
    uint8_t personality;
    uint8_t label_len;
    char label[DEVICE_LABEL_MAX];
    uint8_t crc;
} settings_t;

extern settings_t g_set;

void settings_load(void);
void settings_changed(void);   /* schedule a non-blocking EEPROM write */
void settings_poll(void);      /* call from main loop: writes 1 byte when EEPROM is ready */
void settings_flush(void);     /* finish all pending writes (before a reset), kicks the WDT */
uint32_t devid_load(void);     /* EEPROM ID, generated once if blank */
