/* ui_gfx.c - display list + strip renderer with per-tile dirty detection (pure C11). */
#include "ui_gfx.h"

#include <string.h>

#include "ui_font.h"

#define MAX_PRIMS 320
#define POOL_SIZE 3072

enum { P_FILL, P_TEXT };

typedef struct {
    uint8_t kind;
    uint8_t font;
    uint8_t scale;
    uint16_t color;
    int16_t x, y, w, h; /* bounding box */
    uint16_t str;       /* offset into the pool */
    uint16_t len;
} prim_t;

static struct {
    prim_t prims[MAX_PRIMS];
    char pool[POOL_SIZE];
    uint16_t n_prims;
    uint16_t pool_used;
    bool overflow;
    uint16_t *strip;
    bool swap;
    ui_gfx_blit_fn blit;
    void *ctx;
    uint32_t tile_crc[UI_TILES_Y][UI_TILES_X];
    bool valid;
} G;

static const ui_font_t *font_of(ui_font_id_t f) { return f == UI_FONT_S ? &ui_font_6x8 : &ui_font_8x16b; }

int ui_font_w(ui_font_id_t f) { return font_of(f)->w; }
int ui_font_h(ui_font_id_t f) { return font_of(f)->h; }

void ui_gfx_init(uint16_t *strip, bool swap, ui_gfx_blit_fn blit, void *ctx)
{
    G.strip = strip;
    G.swap = swap;
    G.blit = blit;
    G.ctx = ctx;
    G.valid = false;
    ui_gfx_begin();
}

void ui_gfx_begin(void)
{
    G.n_prims = 0;
    G.pool_used = 0;
    G.overflow = false;
}

bool ui_gfx_overflow(void) { return G.overflow; }

static prim_t *add(void)
{
    if (G.n_prims >= MAX_PRIMS) {
        G.overflow = true;
        return NULL;
    }
    return &G.prims[G.n_prims++];
}

void ui_fill(int x, int y, int w, int h, uint16_t c)
{
    if (w <= 0 || h <= 0) return;
    prim_t *p = add();
    if (!p) return;
    *p = (prim_t){.kind = P_FILL, .color = c, .x = (int16_t)x, .y = (int16_t)y, .w = (int16_t)w, .h = (int16_t)h};
}

void ui_frame(int x, int y, int w, int h, uint16_t c)
{
    ui_fill(x, y, w, 1, c);
    ui_fill(x, y + h - 1, w, 1, c);
    ui_fill(x, y, 1, h, c);
    ui_fill(x + w - 1, y, 1, h, c);
}

int ui_text_width(ui_font_id_t f, const char *s) { return (int)strlen(s) * font_of(f)->w; }

int ui_text_s(int x, int y, ui_font_id_t f, int scale, uint16_t fg, const char *s)
{
    const ui_font_t *fn = font_of(f);
    size_t len = strlen(s);
    if (scale < 1) scale = 1;
    int w = (int)len * fn->w * scale;
    if (len == 0) return x;
    if (G.pool_used + len > POOL_SIZE) {
        G.overflow = true;
        return x + w;
    }
    prim_t *p = add();
    if (!p) return x + w;
    memcpy(&G.pool[G.pool_used], s, len);
    *p = (prim_t){.kind = P_TEXT, .font = (uint8_t)f, .scale = (uint8_t)scale, .color = fg, .x = (int16_t)x,
                  .y = (int16_t)y, .w = (int16_t)w, .h = (int16_t)(fn->h * scale), .str = G.pool_used,
                  .len = (uint16_t)len};
    G.pool_used = (uint16_t)(G.pool_used + len);
    return x + w;
}

void ui_text_r(int x_end, int y, ui_font_id_t f, uint16_t fg, const char *s)
{
    ui_text(x_end - ui_text_width(f, s), y, f, fg, s);
}

void ui_text_c(int x, int w, int y, ui_font_id_t f, int scale, uint16_t fg, const char *s)
{
    int tw = ui_text_width(f, s) * scale;
    ui_text_s(x + (w - tw) / 2, y, f, scale, fg, s);
}

/* ------------------------------------------------------------------------------------------------ raster */

static inline uint16_t px_val(uint16_t c) { return G.swap ? (uint16_t)((c >> 8) | (c << 8)) : c; }

/* Draw primitive p into the strip covering rows [y0, y0 + UI_STRIP_H). */
static void raster(const prim_t *p, int y0)
{
    int ys = p->y > y0 ? p->y : y0;
    int ye = p->y + p->h < y0 + UI_STRIP_H ? p->y + p->h : y0 + UI_STRIP_H;
    int xs = p->x > 0 ? p->x : 0;
    int xe = p->x + p->w < UI_W ? p->x + p->w : UI_W;
    if (ys >= ye || xs >= xe) return;
    uint16_t c = px_val(p->color);
    if (p->kind == P_FILL) {
        for (int y = ys; y < ye; y++) {
            uint16_t *row = &G.strip[(y - y0) * UI_W];
            for (int x = xs; x < xe; x++) row[x] = c;
        }
        return;
    }
    const ui_font_t *fn = font_of((ui_font_id_t)p->font);
    int sc = p->scale;
    int gw = fn->w * sc;
    for (int y = ys; y < ye; y++) {
        int gy = (y - p->y) / sc;
        uint16_t *row = &G.strip[(y - y0) * UI_W];
        for (int i = 0; i < p->len; i++) {
            int gx0 = p->x + i * gw;
            if (gx0 + gw <= xs || gx0 >= xe) continue;
            uint8_t ch = (uint8_t)G.pool[p->str + i];
            if (ch < fn->first || ch >= fn->first + fn->count) ch = '?';
            uint8_t bits = fn->bits[(ch - fn->first) * fn->h + gy];
            if (!bits) continue;
            for (int bx = 0; bx < fn->w; bx++) {
                if (!(bits & (0x80u >> bx))) continue;
                for (int s = 0; s < sc; s++) {
                    int x = gx0 + bx * sc + s;
                    if (x >= xs && x < xe) row[x] = c;
                }
            }
        }
    }
}

static uint32_t tile_hash(int tx)
{
    uint32_t h = 2166136261u;
    for (int y = 0; y < UI_STRIP_H; y++) {
        const uint16_t *row = &G.strip[y * UI_W + tx * UI_TILE_W];
        for (int x = 0; x < UI_TILE_W; x++) {
            h = (h ^ row[x]) * 16777619u;
        }
    }
    return h;
}

uint32_t ui_gfx_render(bool force)
{
    uint32_t sent = 0;
    if (!G.strip || !G.blit) return 0;
    if (!G.valid) force = true;
    for (int ty = 0; ty < UI_TILES_Y; ty++) {
        int y0 = ty * UI_STRIP_H;
        memset(G.strip, 0, sizeof(uint16_t) * UI_W * UI_STRIP_H);
        for (int i = 0; i < G.n_prims; i++) raster(&G.prims[i], y0);
        int first = -1, last = -1;
        for (int tx = 0; tx < UI_TILES_X; tx++) {
            uint32_t h = tile_hash(tx);
            if (force || h != G.tile_crc[ty][tx]) {
                G.tile_crc[ty][tx] = h;
                if (first < 0) first = tx;
                last = tx;
            }
        }
        if (first < 0) continue;
        int x0 = first * UI_TILE_W;
        int w = (last - first + 1) * UI_TILE_W;
        if (w != UI_W) {
            /* compact the dirty span to the start of the buffer (row by row, forward: dst <= src) */
            for (int y = 0; y < UI_STRIP_H; y++) {
                memmove(&G.strip[y * w], &G.strip[y * UI_W + x0], sizeof(uint16_t) * (size_t)w);
            }
        }
        G.blit(G.ctx, x0, y0, w, UI_STRIP_H, G.strip);
        sent += (uint32_t)(w * UI_STRIP_H);
    }
    G.valid = true;
    return sent;
}
