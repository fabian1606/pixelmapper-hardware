/*
 * LED output driver API. Backends:
 *   output_gpio_bcm.c  - direct GPIO, Binary Code Modulation on Timer1 (now)
 *   (later)            - shift register / LED driver IC
 * The backend is selected with OUTPUT_BACKEND in config.h.
 */
#pragma once
#include <stdint.h>
#include "config.h"

void out_init(void);                          /* pins + timer, all off */
void out_write(const uint8_t lv[LED_CHANNELS]); /* 8-bit linear levels, applied next PWM period */
