/*
 * ui_input.h - encoder/button gestures (pure C11). Fed every few ms with encoder detents and raw button levels.
 *
 * Gestures (docs/ui-konzept.md 5.1):
 *   turn           UI_KEY_ROT, steps +-n, accel x1/x10/x100 from the detent interval (reset on direction change)
 *   push short     UI_KEY_PUSH on release (< 600 ms, not consumed by a hold action)
 *   BACK short     UI_KEY_BACK on release (< 1 s)
 *   BACK long      UI_KEY_BACK_LONG once after 1 s held
 *   BACK + push    held together for 3 s: UI_KEY_COMBO once (panel lock toggle); releases after a combo emit nothing
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define UI_PUSH_SHORT_MS 600u
#define UI_BACK_LONG_MS 1000u
#define UI_COMBO_MS 3000u

typedef enum {
    UI_KEY_NONE = 0,
    UI_KEY_ROT,
    UI_KEY_PUSH,
    UI_KEY_BACK,
    UI_KEY_BACK_LONG,
    UI_KEY_COMBO,
} ui_key_type_t;

typedef struct {
    uint8_t type;   /**< ui_key_type_t */
    int8_t steps;   /**< UI_KEY_ROT: detents (+ = clockwise) */
    uint8_t accel;  /**< UI_KEY_ROT: 1, 10 or 100 */
} ui_key_t;

typedef struct {
    bool push, back;              /* debounced levels */
    uint32_t push_t, back_t;      /* press time */
    bool push_consumed;           /* hold action took the push: no UI_KEY_PUSH on release */
    bool back_long_sent;
    bool combo;                   /* both pressed together */
    bool combo_sent;
    uint32_t combo_t;
    int last_dir;
    uint32_t det_t[3];            /* times of the last detents */
    uint8_t q_head, q_tail;
    ui_key_t q[16];
} ui_input_t;

void ui_input_init(ui_input_t *in);
/** detents since the last call (+ = clockwise), button levels (true = pressed, already debounced), time. */
void ui_input_feed(ui_input_t *in, int detents, bool push, bool back, uint32_t now_ms);
bool ui_input_pop(ui_input_t *in, ui_key_t *out);
/** How long push has been held alone (0 = not pressed or part of a combo). */
uint32_t ui_input_push_held(const ui_input_t *in, uint32_t now_ms);
/** Mark the current push as used by a hold action (no UI_KEY_PUSH on release). */
void ui_input_consume_push(ui_input_t *in);
/** Progress of the BACK + push combo in ms (0 = not active, stops at UI_COMBO_MS). */
uint32_t ui_input_combo_ms(const ui_input_t *in, uint32_t now_ms);
