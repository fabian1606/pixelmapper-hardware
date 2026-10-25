/*
 * ui_gfx.h - display list + strip renderer of the front panel (pure C11, no ESP-IDF).
 *
 * Screens describe a whole frame as a display list (fills and text, painter's order) every time they are drawn.
 * ui_gfx_render() rasterizes it strip by strip (160 x 16 px = 5 KB, one buffer) and compares a CRC per 32 x 16 px
 * tile with the previous frame. Only the dirty span of each strip is sent to the backend (one CASET/RASET window),
 * so a frame that did not change costs CPU but no SPI traffic. The backend is a function pointer: the ST7735S
 * driver on the target, a framebuffer + PPM writer in the host renderer.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define UI_W 160
#define UI_H 128
#define UI_STRIP_H 16
#define UI_TILE_W 32
#define UI_TILES_X (UI_W / UI_TILE_W)
#define UI_TILES_Y (UI_H / UI_STRIP_H)

/** RGB565 from 8-bit components. */
#define UI_RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

/* Colour system (docs/ui-konzept.md 2.4): black background, only very light or saturated colours. */
#define UI_C_BLACK UI_RGB(0, 0, 0)
#define UI_C_WHITE UI_RGB(255, 255, 255)
#define UI_C_GRAY UI_RGB(0xA0, 0xA0, 0xA0)   /**< secondary text */
#define UI_C_DIM UI_RGB(0x60, 0x60, 0x60)    /**< disabled / off */
#define UI_C_LINE UI_RGB(0x50, 0x50, 0x50)   /**< separator lines only */
#define UI_C_CYAN UI_RGB(0x00, 0xDC, 0xFF)   /**< editing / staged / update */
#define UI_C_ORANGE UI_RGB(0xFF, 0x8C, 0x00) /**< changed remotely */
#define UI_C_YELLOW UI_RGB(0xFF, 0xE0, 0x00) /**< warning, data loss */
#define UI_C_RED UI_RGB(0xFF, 0x20, 0x20)    /**< error, danger */
#define UI_C_GREEN UI_RGB(0x20, 0xFF, 0x20)  /**< output sending */
#define UI_C_BLUE UI_RGB(0x30, 0x90, 0xFF)   /**< input receiving */
#define UI_C_VIOLET UI_RGB(0xC0, 0x40, 0xFF) /**< RDM activity */

typedef enum {
    UI_FONT_N = 0, /**< 8x16 bold */
    UI_FONT_S = 1, /**< 6x8 */
} ui_font_id_t;

/** Backend: write a w x h rectangle. Pixels in the byte order chosen with ui_gfx_init(swap). */
typedef void (*ui_gfx_blit_fn)(void *ctx, int x, int y, int w, int h, const uint16_t *px);

/** `swap` = store pixels big-endian (SPI wire order). `strip` = buffer of UI_W * UI_STRIP_H pixels (DMA-capable on
 *  the target). */
void ui_gfx_init(uint16_t *strip, bool swap, ui_gfx_blit_fn blit, void *ctx);

/** Start a new display list. */
void ui_gfx_begin(void);
void ui_fill(int x, int y, int w, int h, uint16_t c);
/** 1-px outline. */
void ui_frame(int x, int y, int w, int h, uint16_t c);
/** Transparent text (only set pixels are drawn), `scale` 1 or 2. Returns the x after the text. */
int ui_text_s(int x, int y, ui_font_id_t f, int scale, uint16_t fg, const char *s);
static inline int ui_text(int x, int y, ui_font_id_t f, uint16_t fg, const char *s)
{
    return ui_text_s(x, y, f, 1, fg, s);
}
/** Text right-aligned so that it ends at x_end (exclusive). */
void ui_text_r(int x_end, int y, ui_font_id_t f, uint16_t fg, const char *s);
/** Text centred in [x, x + w). */
void ui_text_c(int x, int w, int y, ui_font_id_t f, int scale, uint16_t fg, const char *s);
int ui_text_width(ui_font_id_t f, const char *s);
int ui_font_w(ui_font_id_t f);
int ui_font_h(ui_font_id_t f);

/** Rasterize and send what changed since the last call. `force` resends everything (after panel power-up).
 *  Returns the number of pixels sent. */
uint32_t ui_gfx_render(bool force);

/** True when the display list overflowed in the last frame (debug). */
bool ui_gfx_overflow(void);
