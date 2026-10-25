/*
 * lcd_st7735.h - ST7735S 128x160 TFT over SPI (ESP-IDF, DMA), used in landscape (160 x 128).
 *
 * Pins/host/rotation come from pm_board_display_pins_t. Coordinates are always landscape: x 0..159, y 0..127.
 * Pixel data is RGB565 in wire order (big-endian: high byte first in memory), so the renderer can hand its strip
 * buffer to DMA unchanged. The buffer must be DMA-capable (internal RAM).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "pm_board.h"

#define LCD_W 160
#define LCD_H 128

/** Init SPI bus (DMA, SPI_DMA_CH_AUTO), panel (reset, SLPOUT, COLMOD 16 bit, MADCTL per rotation, gamma,
 *  INVOFF, NORON, DISPON) and the backlight PWM (LEDC, off until lcd_st7735_backlight()). `spi_hz` e.g. 20 MHz.
 *  `max_transfer` = largest blit in bytes (strip buffer size). */
esp_err_t lcd_st7735_init(const pm_board_display_pins_t *pins, uint32_t spi_hz, int max_transfer);

/** Write a w x h rectangle at (x, y) (CASET/RASET/RAMWR + data via DMA). Blocks until the transfer is done, so
 *  the caller may reuse `px` immediately. */
esp_err_t lcd_st7735_blit(int x, int y, int w, int h, const uint16_t *px);

/** Backlight 0..255 (0 = off). Gamma-corrected PWM is the caller's business; this is linear duty. */
void lcd_st7735_backlight(uint8_t level);

/** Panel on/off (DISPON/DISPOFF; SLPIN/SLPOUT with the required delays). Off keeps the controller RAM. */
void lcd_st7735_power(bool on);
